#include "linear_cycle.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"

#include "esphome/components/json/json_util.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome::linear_cycle {

static const char *const TAG = "linear_cycle";

static const char *const API_URL = "https://api.linear.app/graphql";
static const size_t MAX_RESPONSE_BYTES = 32 * 1024;

static const Color WHITE = Color(0xFFFFFF);
static const Color GREY = Color(0xA7A7A7);
static const Color DIM = Color(0x3C3C3C);
// Linear's default colours, used when a state's own colour can't be read
static const Color DEFAULT_STATE_COLOR = Color(0xBEC2C8);
static const Color DEFAULT_DONE_COLOR = Color(0x5E6AD2);

// Layout, top to bottom: title, status and ticket with the project on the
// right, the progress bar, then the cycle and page number
static const int TITLE_BASELINE = 8;
static const int DETAIL_TOP = 11;
static const int DETAIL_BASELINE = 19;
static const int BAR_Y = 21;
static const int FOOTER_BASELINE = 31;
static const int TICKET_X = 9;

// Lower numbers show first; any other state (done, cancelled) isn't shown
static int state_order(const std::string &type) {
  if (type == "started") return 0;
  if (type == "unstarted") return 1;
  if (type == "backlog") return 2;
  if (type == "triage") return 3;
  return -1;
}

static Color parse_color(const char *str, Color fallback) {
  if (str == nullptr || *str == '\0') return fallback;
  if (*str == '#') str++;
  char *end = nullptr;
  unsigned long value = std::strtoul(str, &end, 16);
  return *end == '\0' ? Color(static_cast<uint32_t>(value)) : fallback;
}

static time_t parse_utc(const char *str) {
  int year, month, day, hour, minute, second;
  if (str == nullptr || sscanf(str, "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute, &second) != 6) {
    return 0;
  }
  // Days since 1970-01-01, from days_from_civil
  year -= month <= 2;
  int era = (year >= 0 ? year : year - 399) / 400;
  int year_of_era = year - era * 400;
  int day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  int day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
  long days = static_cast<long>(era) * 146097 + day_of_era - 719468;
  return static_cast<time_t>(days) * 86400 + hour * 3600 + minute * 60 + second;
}

void LinearCycle::setup() {
  // TLS needs more stack than a plain request
  xTaskCreate(&LinearCycle::fetch_task_, "linear_cycle", 12288, this, 1, &this->fetch_task_handle_);
}

void LinearCycle::update() {
  if (this->fetch_task_handle_ != nullptr) {
    xTaskNotifyGive(this->fetch_task_handle_);
  }
}

void LinearCycle::dump_config() {
  ESP_LOGCONFIG(TAG, "Linear Cycle:");
  ESP_LOGCONFIG(TAG, "  Team: %s", this->team_key_.empty() ? "(first active cycle)" : this->team_key_.c_str());
  LOG_UPDATE_INTERVAL(this);
}

void LinearCycle::fetch_task_(void *arg) {
  auto *self = static_cast<LinearCycle *>(arg);
  while (true) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    self->fetch_();
  }
}

std::string LinearCycle::build_request_() {
  std::string filter = "isActive: { eq: true }";
  if (!this->team_key_.empty()) {
    filter += str_sprintf(", team: { key: { eq: \"%s\" } }", this->team_key_.c_str());
  }
  // Only what the board draws, to keep the response small
  std::string query = "query { cycles(first: 1, filter: { " + filter +
                      " }) { nodes { number endsAt issues(first: 100) { nodes { "
                      "identifier title state { type color } project { name } } } } } }";
  return json::build_json([&query](JsonObject root) { root["query"] = query; });
}

void LinearCycle::fetch_() {
  if (this->api_key_.empty()) {
    return;
  }

  esp_http_client_config_t config = {};
  config.url = API_URL;
  config.method = HTTP_METHOD_POST;
  config.timeout_ms = 10000;
  config.crt_bundle_attach = esp_crt_bundle_attach;

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == nullptr) {
    ESP_LOGW(TAG, "Couldn't create HTTP client");
    return;
  }

  esp_http_client_set_header(client, "Authorization", this->api_key_.c_str());
  esp_http_client_set_header(client, "Content-Type", "application/json");

  std::string request = this->build_request_();
  std::string body;
  esp_err_t err = esp_http_client_open(client, request.size());
  if (err == ESP_OK && esp_http_client_write(client, request.c_str(), request.size()) == (int) request.size()) {
    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status == 200) {
      char buffer[512];
      int read;
      while ((read = esp_http_client_read(client, buffer, sizeof(buffer))) > 0) {
        if (body.size() + read > MAX_RESPONSE_BYTES) {
          ESP_LOGW(TAG, "Response too large, skipping");
          body.clear();
          break;
        }
        body.append(buffer, read);
      }
    } else {
      ESP_LOGW(TAG, "Linear returned HTTP %d", status);
    }
  } else {
    ESP_LOGW(TAG, "Request failed: %s", esp_err_to_name(err));
  }
  esp_http_client_close(client);
  esp_http_client_cleanup(client);

  if (!body.empty()) {
    this->parse_(body);
  }
}

void LinearCycle::parse_(const std::string &body) {
  auto data = std::make_shared<CycleData>();

  bool valid = json::parse_json(body, [&data](JsonObject root) -> bool {
    if (root["errors"].is<JsonArray>()) {
      ESP_LOGW(TAG, "Linear error: %s", root["errors"][0]["message"] | "(no message)");
      return false;
    }
    JsonArray cycles = root["data"]["cycles"]["nodes"];
    if (cycles.isNull()) {
      return false;
    }
    if (cycles.size() == 0) {
      return true;  // No active cycle
    }

    JsonObject cycle = cycles[0];
    data->active = true;
    data->number = cycle["number"] | 0;
    data->ends_at = parse_utc(cycle["endsAt"] | (const char *) nullptr);
    data->done_color = DEFAULT_DONE_COLOR;

    JsonArray issues = cycle["issues"]["nodes"];
    for (JsonObject node : issues) {
      std::string type = node["state"]["type"] | "";
      if (type == "canceled" || type == "duplicate") {
        continue;
      }
      data->total++;
      if (type == "completed") {
        data->done++;
        data->done_color = parse_color(node["state"]["color"] | "", DEFAULT_DONE_COLOR);
        continue;
      }
      if (state_order(type) < 0) {
        continue;
      }
      Issue issue;
      issue.id = node["identifier"] | "";
      issue.title = node["title"] | "";
      issue.project = node["project"]["name"] | "";
      issue.state_type = type;
      issue.state_color = parse_color(node["state"]["color"] | "", DEFAULT_STATE_COLOR);
      data->open.push_back(std::move(issue));
    }

    std::stable_sort(data->open.begin(), data->open.end(), [](const Issue &a, const Issue &b) {
      return state_order(a.state_type) < state_order(b.state_type);
    });
    return true;
  });

  if (!valid) {
    ESP_LOGW(TAG, "Couldn't read Linear's response");
    return;
  }

  ESP_LOGD(TAG, "Cycle %d: %d of %d done, %u open", data->number, data->done, data->total,
           (unsigned) data->open.size());
  std::lock_guard<std::mutex> lock(this->data_mutex_);
  this->data_ = data;
}

std::vector<LinearCycle::Piece> LinearCycle::pieces_(const std::string &text) {
  // Small caps: runs of lowercase letters become capitals in the smaller font
  std::vector<Piece> pieces;
  for (char c : text) {
    bool lower = c >= 'a' && c <= 'z';
    font::Font *font = lower ? this->small_font_ : this->font_;
    char ch = lower ? static_cast<char>(c - 'a' + 'A') : c;
    if (pieces.empty() || pieces.back().font != font) {
      pieces.push_back({font, std::string(1, ch)});
    } else {
      pieces.back().text += ch;
    }
  }
  return pieces;
}

int LinearCycle::text_width_(const std::string &text) {
  int total = 0, width, _;
  for (const auto &piece : this->pieces_(text)) {
    piece.font->measure(piece.text.c_str(), &width, &_, &_, &_);
    total += width;
  }
  return total;
}

void LinearCycle::draw_text_(const std::string &text, int x, int baseline, Color color) {
  int width, _;
  for (const auto &piece : this->pieces_(text)) {
    // Lining up on the baseline lets both fonts' capitals sit on the same row
    this->display_->print(x, baseline, piece.font, color, display::TextAlign::BASELINE_LEFT, piece.text.c_str());
    piece.font->measure(piece.text.c_str(), &width, &_, &_, &_);
    x += width;
  }
}

void LinearCycle::draw_status_(const std::string &state_type, int x, int y, Color color) {
  // 7x7 circles in Linear's style: half-filled for in progress, empty otherwise
  static const char *const RING[] = {"..###..", ".#...#.", "#.....#", "#.....#", "#.....#", ".#...#.", "..###.."};
  static const char *const HALF[] = {"..###..", ".#.###.", "#..####", "#..####", "#..####", ".#.###.", "..###.."};
  const char *const *shape = state_type == "started" ? HALF : RING;
  for (int row = 0; row < 7; row++) {
    for (int col = 0; col < 7; col++) {
      if (shape[row][col] == '#') {
        this->display_->draw_pixel_at(x + col, y + row, color);
      }
    }
  }
}

void LinearCycle::draw_message_(const char *message) {
  this->display_->print(this->display_->get_width() / 2, this->display_->get_height() / 2, this->font_, GREY,
                        display::TextAlign::CENTER, message);
}

void LinearCycle::draw() {
  std::shared_ptr<const CycleData> data;
  {
    std::lock_guard<std::mutex> lock(this->data_mutex_);
    data = this->data_;
  }
  if (!data) {
    this->draw_message_("Loading Linear...");
    return;
  }
  if (!data->active) {
    this->draw_message_("No active cycle");
    return;
  }

  auto *d = this->display_;
  const int width = d->get_width();

  // Footer and progress bar, shown even when nothing is left to do
  int x = width;
  if (!data->open.empty()) {
    if (this->page_ >= data->open.size()) {
      this->page_ = 0;
    }
    std::string page = str_sprintf("%u of %u", (unsigned) this->page_ + 1, (unsigned) data->open.size());
    x = width - this->text_width_(page);
    this->draw_text_(page, x, FOOTER_BASELINE, GREY);
  }
  std::string cycle = str_sprintf("Cycle %d", data->number);
  int cycle_end = this->text_width_(cycle);
  this->draw_text_(cycle, 0, FOOTER_BASELINE, WHITE);
  ESPTime now = this->rtc_->now();
  if (now.is_valid() && data->ends_at > now.timestamp) {
    long seconds = data->ends_at - now.timestamp;
    std::string left = seconds >= 86400 ? str_sprintf("%ldd left", seconds / 86400)
                                        : str_sprintf("%ldh left", seconds / 3600);
    this->draw_text_(left, cycle_end + 5, FOOTER_BASELINE, GREY);
  }

  int filled = data->total > 0 ? (width * data->done + data->total / 2) / data->total : 0;
  for (int px = 0; px < width; px++) {
    d->draw_pixel_at(px, BAR_Y, px < filled ? data->done_color : DIM);
  }

  if (data->open.empty()) {
    d->print(width / 2, TITLE_BASELINE + 4, this->font_, GREY, display::TextAlign::BASELINE_CENTER,
             "Nothing left to do");
    return;
  }

  const Issue &issue = data->open[this->page_];

  // Where the project goes: right-aligned after the ticket number, or from
  // just after it when the name is too long to fit (it then scrolls)
  int project_left = TICKET_X + this->text_width_(issue.id) + 6;
  int project_width = this->text_width_(issue.project);
  int project_overflow = std::max(0, project_width - (width - project_left));
  int project_x = project_overflow > 0 ? project_left : width - project_width;
  int title_overflow = std::max(0, this->text_width_(issue.title) - width);

  // Page timing: pause, scroll to the end, pause, scroll back, then a last
  // pause before the next issue
  int overflow = std::max(title_overflow, project_overflow);
  uint32_t scroll_ms = static_cast<uint32_t>(overflow) * 1000 / this->scroll_speed_;
  uint32_t page_ms = overflow > 0 ? this->pause_before_scroll_ + scroll_ms + this->pause_before_reverse_ +
                                        scroll_ms + this->pause_before_next_
                                  : this->pause_before_scroll_ + this->pause_before_next_;
  uint32_t elapsed = millis() - this->page_started_ms_;
  if (elapsed >= page_ms) {
    this->page_ = (this->page_ + 1) % data->open.size();
    this->page_started_ms_ = millis();
    elapsed = 0;
    // The next issue is drawn on the next frame, with its own timing
  }

  int scrolled = 0;
  if (overflow > 0 && elapsed >= this->pause_before_scroll_) {
    uint32_t t = elapsed - this->pause_before_scroll_;
    if (t < scroll_ms) {
      scrolled = t * this->scroll_speed_ / 1000;
    } else if (t < scroll_ms + this->pause_before_reverse_) {
      scrolled = overflow;
    } else if (t < 2 * scroll_ms + this->pause_before_reverse_) {
      scrolled = overflow - (t - scroll_ms - this->pause_before_reverse_) * this->scroll_speed_ / 1000;
    }
  }

  // Title, stopping above the details line so nothing hangs into it
  d->start_clipping(0, 0, width, DETAIL_TOP - 1);
  this->draw_text_(issue.title, -std::min(scrolled, title_overflow), TITLE_BASELINE, WHITE);
  d->end_clipping();

  this->draw_status_(issue.state_type, 0, DETAIL_TOP, issue.state_color);
  this->draw_text_(issue.id, TICKET_X, DETAIL_BASELINE, issue.state_color);
  if (!issue.project.empty()) {
    d->start_clipping(project_left, DETAIL_TOP, width, BAR_Y);
    this->draw_text_(issue.project, project_x - std::min(scrolled, project_overflow), DETAIL_BASELINE, GREY);
    d->end_clipping();
  }
}

}  // namespace esphome::linear_cycle
