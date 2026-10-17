import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import esp32
from esphome.components.display import Display
from esphome.components.font import Font
from esphome.components.time import RealTimeClock
from esphome.const import CONF_ID, CONF_DISPLAY_ID, CONF_TIME_ID

DEPENDENCIES = ["network", "display", "font", "time", "esp32"]
AUTO_LOAD = ["json"]

linear_cycle_ns = cg.esphome_ns.namespace("linear_cycle")
LinearCycle = linear_cycle_ns.class_("LinearCycle", cg.PollingComponent)

CONF_API_KEY = "api_key"
CONF_TEAM_KEY = "team_key"
CONF_FONT_ID = "font_id"
CONF_SMALL_FONT_ID = "small_font_id"
CONF_SCROLL_SPEED = "scroll_speed"
CONF_PAUSE_BEFORE_SCROLL = "pause_before_scroll"
CONF_PAUSE_BEFORE_REVERSE = "pause_before_reverse"
CONF_PAUSE_BEFORE_NEXT = "pause_before_next"

CONFIG_SCHEMA = cv.All(
    cv.only_on_esp32,
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(LinearCycle),
            cv.GenerateID(CONF_DISPLAY_ID): cv.use_id(Display),
            cv.GenerateID(CONF_TIME_ID): cv.use_id(RealTimeClock),
            cv.Required(CONF_API_KEY): cv.string_strict,
            # Only this team's active cycle - otherwise the first active cycle Linear returns
            cv.Optional(CONF_TEAM_KEY): cv.string_strict,
            # The main font
            cv.Required(CONF_FONT_ID): cv.use_id(Font),
            # Lowercase letters are drawn as small caps in this font
            cv.Required(CONF_SMALL_FONT_ID): cv.use_id(Font),
            # Pixels per second
            cv.Optional(CONF_SCROLL_SPEED, default=10): cv.int_range(min=1, max=100),
            cv.Optional(
                CONF_PAUSE_BEFORE_SCROLL, default="5s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(
                CONF_PAUSE_BEFORE_REVERSE, default="1s"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(
                CONF_PAUSE_BEFORE_NEXT, default="2s"
            ): cv.positive_time_period_milliseconds,
        }
    ).extend(cv.polling_component_schema("60s")),
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_display(await cg.get_variable(config[CONF_DISPLAY_ID])))
    cg.add(var.set_rtc(await cg.get_variable(config[CONF_TIME_ID])))
    cg.add(var.set_font(await cg.get_variable(config[CONF_FONT_ID])))
    cg.add(var.set_small_font(await cg.get_variable(config[CONF_SMALL_FONT_ID])))

    cg.add(var.set_api_key(config[CONF_API_KEY]))
    if CONF_TEAM_KEY in config:
        cg.add(var.set_team_key(config[CONF_TEAM_KEY]))

    cg.add(var.set_scroll_speed(config[CONF_SCROLL_SPEED]))
    cg.add(var.set_pause_before_scroll(config[CONF_PAUSE_BEFORE_SCROLL].total_milliseconds))
    cg.add(var.set_pause_before_reverse(config[CONF_PAUSE_BEFORE_REVERSE].total_milliseconds))
    cg.add(var.set_pause_before_next(config[CONF_PAUSE_BEFORE_NEXT].total_milliseconds))

    # Linear is only reachable over https
    esp32.add_idf_sdkconfig_option("CONFIG_MBEDTLS_CERTIFICATE_BUNDLE", True)
