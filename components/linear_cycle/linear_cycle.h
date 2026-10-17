#pragma once

#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "esphome/core/component.h"
#include "esphome/components/display/display.h"
#include "esphome/components/font/font.h"
#include "esphome/components/time/real_time_clock.h"

namespace esphome::linear_cycle {

struct Issue {
  // e.g. "ABC-123"
  std::string id;
  std::string title;
  // Empty when the issue isn't in a project
  std::string project;
  // Linear's state type: started, unstarted, backlog or triage
  std::string state_type;
  Color state_color;
};

struct CycleData {
  // False when the team has no active cycle
  bool active{false};
  int number{0};
  time_t ends_at{0};
  int done{0};
  // Every issue in the cycle apart from cancelled ones
  int total{0};
  Color done_color;
  // Issues still to do, in progress first
  std::vector<Issue> open;
};

class LinearCycle : public PollingComponent {
 public:
  void setup() override;
  void update() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  // Draws the current page
  void draw();

  void set_display(display::Display *display) { this->display_ = display; }
  void set_rtc(time::RealTimeClock *rtc) { this->rtc_ = rtc; }
  void set_font(font::Font *font) { this->font_ = font; }
  void set_small_font(font::Font *font) { this->small_font_ = font; }
  void set_api_key(const std::string &api_key) { this->api_key_ = api_key; }
  void set_team_key(const std::string &team_key) { this->team_key_ = team_key; }
  void set_scroll_speed(int pixels_per_second) { this->scroll_speed_ = pixels_per_second; }
  void set_pause_before_scroll(uint32_t ms) { this->pause_before_scroll_ = ms; }
  void set_pause_before_reverse(uint32_t ms) { this->pause_before_reverse_ = ms; }
  void set_pause_before_next(uint32_t ms) { this->pause_before_next_ = ms; }

 protected:
  // A run of text in one font
  struct Piece {
    font::Font *font;
    std::string text;
  };

  static void fetch_task_(void *arg);
  void fetch_();
  std::string build_request_();
  void parse_(const std::string &body);

  std::vector<Piece> pieces_(const std::string &text);
  int text_width_(const std::string &text);
  void draw_text_(const std::string &text, int x, int baseline, Color color);
  void draw_status_(const std::string &state_type, int x, int y, Color color);
  void draw_message_(const char *message);

  display::Display *display_{nullptr};
  time::RealTimeClock *rtc_{nullptr};
  font::Font *font_{nullptr};
  font::Font *small_font_{nullptr};

  std::string api_key_;
  std::string team_key_;

  int scroll_speed_{10};
  uint32_t pause_before_scroll_{5000};
  uint32_t pause_before_reverse_{1000};
  uint32_t pause_before_next_{2000};

  std::mutex data_mutex_;
  std::shared_ptr<const CycleData> data_;

  // Which issue is on display, and since when
  size_t page_{0};
  uint32_t page_started_ms_{0};

  TaskHandle_t fetch_task_handle_{nullptr};
};

}  // namespace esphome::linear_cycle
