#pragma once
#include "esphome/core/defines.h"

#include "esphome/core/component.h"
#include "esphome/core/preferences.h"   // ⬅️ kasuta ESPHome püsisalvestust
#include "esphome/core/helpers.h"
#include "esphome/components/web_server_base/web_server_base.h"
#include "esphome/components/http_request/http_request.h"

namespace esphome {
namespace moodle_setup {



class MoodleSetup : public AsyncWebHandler, public Component {
 public:
  MoodleSetup(web_server_base::WebServerBase *base);
  MoodleSetup();
  void setup();
  void loop();

  void start();

  // Getterid kasutamiseks YAML-lambdades
  std::string get_user() const { return user; }
  std::string get_password() const { return pass; }   // eelista tokenit
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
    return false;
  }

  void handleRequest(AsyncWebServerRequest *req) override;
  bool isRequestHandlerTrivial() const { return false; };
  ESPPreferenceObject epo_user;
  ESPPreferenceObject epo_pass;
  ESPPreferenceObject epo_token;

  void set_http(esphome::http_request::HttpRequestComponent *h) { this->http_ = h; }

  bool call_ws_(const std::string &url, const std::string &body, std::string &out_json);

  void set_base_url(const std::string &base_url);
  esphome::http_request::HttpRequestComponent *http_{nullptr};

  bool setup_called = false;
 protected:
  web_server_base::WebServerBase *base_;

  bool pump = false;
  std::shared_ptr<esphome::http_request::HttpContainer> conn_;

 private:

  std::string user;
  std::string pass;
  std::string token;

  static String html_escape_(const char *in);
  static String get_param_(AsyncWebServerRequest *req, const char *name);

  void init_prefs_();
  void load_from_prefs_();
  void save_to_prefs_(const std::string &user, const std::string &pass, const std::string &token);


};
}  // namespace moodle_setup
}  // namespace esphome
