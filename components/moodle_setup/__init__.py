import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_ID
from esphome.components import http_request

# Ütle ESPHome'ile, et sõltume web_server'ist
AUTO_LOAD = ["web_server", "http_request"]
DEPENDENCIES = ["web_server", "http_request"]

moodle_ns = cg.esphome_ns.namespace("moodle_setup")
MoodleSetup = moodle_ns.class_("MoodleSetup", cg.Component)

CONF_HTTP = "http_id"
CONF_BASE_URL = "base_url"
CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(MoodleSetup),
    cv.Required(CONF_HTTP): cv.use_id(http_request.HttpRequestComponent),
    cv.Required(CONF_BASE_URL): cv.string,   # keep it simple
})


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    http = await cg.get_variable(config[CONF_HTTP])
    cg.add(var.set_http(http))
    cg.add(var.set_base_url(config[CONF_BASE_URL]))
