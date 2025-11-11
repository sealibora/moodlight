#pragma once
#include "esphome/core/defines.h"

#include "esphome/core/component.h"
#include "esphome/core/preferences.h"   // ⬅️ kasuta ESPHome püsisalvestust
#include "esphome/core/helpers.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/components/http_request/http_request.h"
#include "esphome/components/time/real_time_clock.h"

namespace esphome {
namespace moodle_setup {


  struct MoodleSettings {
    char user[33];
    char password[50];
    char token[50];
  } PACKED;

  enum MoodleState : uint8_t {
    MOODLE_STATE_OFFLINE = 0,
    MOODLE_STATE_START_LOGIN,
    MOODLE_STATE_LOGIN,
    MOODLE_STATE_LOGIN_FAILED,
    MOODLE_STATE_TOKEN_RECEIVED,
    MOODLE_STATE_REQUESTING_TASKS,
    MOODLE_STATE_START_REQUESTING_TASKS,
    MOODLE_STATE_IDLE
  };

  struct MoodleEvent {
    uint32_t id = 0;
    std::string name;
    uint32_t timestamp;
  };


class MoodleSetup : public AsyncWebHandler, public Component {
 public:
  MoodleSetup(web_server_base::WebServerBase *base);
  MoodleSetup();
  void dump_config();
  void setup();
  void loop();

  void start();

  // Getterid kasutamiseks YAML-lambdades
  std::string get_user() const { return user; }
  std::string get_password() const { return password; }   // eelista tokenit
  std::string get_token() const { return token; }

  bool canHandle(AsyncWebServerRequest *request) const override {
    if (request->method() == HTTP_GET) {
      if (request->url() == F("/moodle"))
        return true;
    }
    if (request->method() == HTTP_POST) {
      if (request->url() == F("/moodle/save"))
        return true;
    }
    if (request->method() == HTTP_POST) {
      if (request->url() == F("/moodle/start"))
        return true;
    }
    if (request->method() == HTTP_POST) {
      if (request->url() == F("/moodle/request_tasks"))
        return true;
    }
    return false;
  }

  void handleRequest(AsyncWebServerRequest *req) override;
  bool isRequestHandlerTrivial() const { return false; };
  ESPPreferenceObject pref_;

  void set_http(esphome::http_request::HttpRequestComponent *h) { this->http_ = h; }
  void set_time(esphome::time::RealTimeClock *t) { time_ = t; }

  bool call_ws_(const std::string &url, const std::string &body, std::string &out_json);

  void set_base_url(const std::string &base_url);
  esphome::http_request::HttpRequestComponent *http_{nullptr};

 protected:
  MoodleState state_{MOODLE_STATE_OFFLINE};

  web_server_base::WebServerBase *base_;

  bool pump = false;
  std::shared_ptr<esphome::http_request::HttpContainer> conn_;

 private:

  std::string user;
  std::string password;
  std::string token;

  static String html_escape_(const char *in);
  static String get_param_(AsyncWebServerRequest *req, const char *name);

  void init_prefs_();
  void load_from_prefs_();
  void save_to_prefs_();

  esphome::time::RealTimeClock *time_{nullptr};



};
}  // namespace moodle_setup
}  // namespace esphome
