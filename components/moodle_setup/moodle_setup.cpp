#include "moodle_setup.h"
#include "esphome/core/log.h"
#include <ESPAsyncWebServer.h>
//#include <AsyncTCP.h>

#include <string>

// RFC3986 "unreserved": ALPHA / DIGIT / "-" / "_" / "." / "~"
static inline bool is_unreserved(char c) {
  return (c >= 'A' && c <= 'Z') ||
         (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') ||
         c == '-' || c == '_' || c == '.' || c == '~';
}

static inline char hex_char(uint8_t v) { return v < 10 ? ('0' + v) : ('A' + (v - 10)); }

// space_as_plus=true → " " muutub "+", nagu x-www-form-urlencoded nõuab
static inline std::string url_encode(const std::string &in, bool space_as_plus = true) {
  std::string out;
  out.reserve(in.size() * 3);  // halvim juht
  for (unsigned char c : in) {
    if (is_unreserved((char)c)) {
      out.push_back((char)c);
    } else if (c == ' ' && space_as_plus) {
      out.push_back('+');
    } else {
      out.push_back('%');
      out.push_back(hex_char((c >> 4) & 0xF));
      out.push_back(hex_char(c & 0xF));
    }
  }
  return out;
}


namespace esphome {
namespace moodle_setup {


static const char *const TAG = "moodle_setup";

  // using web_server::global_web_server;
  MoodleSetup::MoodleSetup()  { }

  MoodleSetup::MoodleSetup(web_server_base::WebServerBase *base) : base_(base) { }

  void MoodleSetup::dump_config() {
    ESP_LOGCONFIG(TAG, "user: %s", this->user);
    ESP_LOGCONFIG(TAG, "token: %s", this->token);
  }

void MoodleSetup::setup() {

  // Algväärtusta püsikanalid ja lae salvestatud väärtused
  this->init_prefs_();
  this->load_from_prefs_();

  auto *server = esphome::web_server_base::global_web_server_base;
  if (server == nullptr) {
    ESP_LOGE(TAG, "web_server not initialized");
    return;
  }
  server->add_handler(this);
}

std::string json;


void MoodleSetup::loop() {
  if (this->pump != false) {
    uint8_t buf[256];
    int n = this->conn_->read(buf, 256);
    ESP_LOGI(TAG, "pump n %d", n);
    json.append(reinterpret_cast<const char *>(buf), static_cast<size_t>(n));

    buf[n] = '\0';
    ESP_LOGI(TAG, "buf: %s", buf);
    if (this->conn_->content_length > 0 && this->conn_->get_bytes_read() >= this->conn_->content_length) {
      ESP_LOGI(TAG, "pump OFF");
      esphome::json::parse_json(json, [&](JsonObject root){
        if (root.containsKey("token")) {
          this->token = std::string(root["token"].as<const char*>());
          ESP_LOGI(TAG, "Token OK (len=%u), %s", (unsigned)this->token.size(), this->token.c_str());
          // salvesta eelistustesse, puhasta parool, jne.
          //json.clear();

          return true;
        }
        ESP_LOGW(TAG, "Token error: %s", root["error"] | "unknown");
        return true;
      });

      this->pump = false;
      this->conn_->end();
    }
    // buf[9] = '\0';

    // ESP_LOGI(TAG, "resp code %d", buf);
    // return;
  }
  if (this->setup_called == true) {
    this->setup_called = false;
    ESP_LOGI(TAG, "Loop caught setup_called");

    std::string body = "username=" + url_encode(this->user)
      + "&password=" + url_encode(this->pass)
      + "&service="  + url_encode("moodle_mobile_app"); // või sinu teenus

    if (!this->http_) {
      ESP_LOGI(TAG, "http_ empty");
      return;
    }
    std::list<esphome::http_request::Header> hdr{
      {"Content-Type", "application/x-www-form-urlencoded"}
    };

    auto c = http_->post("https://moodle.tlu.ee/login/token.php", body, hdr);

    
    //auto c = this->http_->get("http://192.168.0.174:8000");
    this->conn_ = c;
    ESP_LOGI(TAG, "HTTP status: %d, CL=%d, read=%u", c->status_code, c->content_length, (unsigned)c->get_bytes_read());
    this->pump = true;
    return;
  }

}


void MoodleSetup::start() {
  //this->base_->add_handler(this);
}
  void MoodleSetup::set_base_url(const std::string &base_url) {

    //this->base_->add_handler(this);
}

void MoodleSetup::handleRequest(AsyncWebServerRequest *req) {
    if (req->url() == F("/moodle")) {

    AsyncResponseStream *stream = req->beginResponseStream(F("text/html"));
    stream->addHeader(F("cache-control"), F("public, max-age=0, must-revalidate"));

    stream->print(F("<!doctype html><html><head><meta charset='utf-8'>"
                      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                      "<title>Moodle Setup</title></head><body>"
                      "<h2>Moodle seadistus</h2>"
                      "<form method='POST' action='/moodle/save'>"
                      "<label>Moodle kasutajanimi<br>"
                    "<input name='user' value=''></label>"));
    stream->print(this->user.c_str());
                  stream->print(F("<br><br>"
                    "<label>Moodle parool<br>"
                    "<input type='password' name='pass' value=''></label>"
                    "<p><em>Soovitus: kasuta parooli asemel web-service tokenit.</em></p>"
                    "<label>Moodle token<br>"
                                  "<input name='token' value=''></label>"));

    stream->print(this->token.c_str());
    stream->print(F("<br><br><button type='submit'>Salvesta</button>"
                    "</form>/moodle</code>"));
    //stream->print(get_mac_address_pretty().c_str());
    //stream->print(F("\",\"name\":\""));
    //stream->print(App.get_name().c_str());
    //stream->print(F("\",\"aps\":[{}"));
    stream->print(F("<form method='POST' action='/moodle/start'>"                     
                    "<button type='submit'>Start</button>"
                    "</form>/moodle</code></p></body></html>"));


    req->send(stream);

    //#ifndef USE_ESP8266
    //AsyncWebServerResponse *response = req->beginResponse(200, "text/html", html);
    //#else
    //AsyncWebServerResponse *response = req->beginResponse_P(200, "text/html", html);
    //#endif
    //static const char HELLO_STR[] PROGMEM = "HELLO WORLD";

    //AsyncWebServerResponse *response2 = req->beginResponse_P(200, "text/html", HELLO_STR);
    //req->send(response);

    }

    if (req->url() == F("/moodle/save")) {

      int n = req->params();
      ESP_LOGI(TAG, "Request params: %d", n);
      for (int i = 0; i < n; i++) {
        auto *p = req->getParam(i);
        if (p->name() == "pass")
          {
            ESP_LOGI(TAG, "[%d] name='%s'  isPost=%d isFile=%d", i, p->name().c_str(), p->isPost(), p->isFile());
            continue;
          }
        ESP_LOGI(TAG, "[%d] name='%s' value='%s' isPost=%d isFile=%d",
                 i, p->name().c_str(), p->value().c_str(), p->isPost(), p->isFile());
      }
    const String user_s = get_param_(req, "user");
    ESP_LOGI(TAG, "user %s", req->arg("user"));   

    const String pass_s = get_param_(req, "pass");
    const String token_s = get_param_(req, "token");

    // Uuenda RAM-is olevaid väärtusi (parooli ei kirjuta üle tühjaga)
    if (user_s.length() > 0) this->user = std::string(user_s.c_str());
    if (pass_s.length() > 0) this->pass = std::string(pass_s.c_str());
    if (token_s.length() > 0) this->token = std::string(token_s.c_str());

    // Kirjuta püsisalvestusse
    this->save_to_prefs_(std::string(user_s.c_str()),
                         std::string(pass_s.c_str()),
                         std::string(token_s.c_str()));

    String resp;
    resp += F("<!doctype html><html><meta charset='utf-8'>"
              "<body><h3>Salvestatud.</h3>"
              "<p>Tagasi <a href='/moodle'>Moodle seadistusse</a>.</p>"
              "</body></html>");
    req->send(200, "text/html; charset=utf-8", resp);
    ESP_LOGI(TAG, "user %s", user_s.c_str());   
    ESP_LOGI(TAG, "Starting connection to Moodle.");
    //this->setup_called = true;
    }

   if (req->url() == F("/moodle/start")) {
     ESP_LOGI(TAG, "POST /moodle/start.");
      this->setup_called = true;
      return;
    }


   ESP_LOGI(TAG, "Endpoints ready: GET /moodle, POST /moodle/save, POST /moodle/start");
   ESP_LOGI(TAG, "setup_called: %d", this->setup_called);
   // auto c = http_->get("http://192.168.0.174:8000");
   // ESP_LOGI(TAG, "c: %d", c);
}

void MoodleSetup::init_prefs_() {
  // Kasutame stabiilseid 32-bit hash-e võtmeks
  // (samad stringid peavad jääma muutumatuks, et säilitatud väärtused säiliks)
  uint32_t k_pref = fnv1_hash("moodle_pref");
  this->pref_ = global_preferences->make_preference<MoodleSettings>(k_pref, true);
}

void MoodleSetup::load_from_prefs_() {
  std::string tmp;

  MoodleSettings save{};
  if (this->pref_.load(&save)) {
    ESP_LOGD(TAG, "Loaded settings: user %s", save.user);
  }

}

void MoodleSetup::save_to_prefs_(const std::string &user, const std::string &pass, const std::string &token) {
  if (!user.empty())  this->epo_user.save(&user);
  if (!pass.empty())  this->epo_pass.save(&pass);
  if (!token.empty()) this->epo_token.save(&token);


}

String MoodleSetup::get_param_(AsyncWebServerRequest *req, const char *name) {
  if (!req->hasParam(name, true)) return "";
  return req->getParam(name, true)->value();
}

String MoodleSetup::html_escape_(const char *in) {
  String out;
  for (const char *p = in; *p; ++p) {
    char c = *p;
    switch (c) {
      case '&': out += F("&amp;"); break;
      case '<': out += F("&lt;"); break;
      case '>': out += F("&gt;"); break;
      case '"': out += F("&quot;"); break;
      case '\'': out += F("&#39;"); break;
      default: out += c;
    }
  }
  return out;
}

  #include "esphome/components/json/json_util.h"
using esphome::json::parse_json;

bool MoodleSetup::call_ws_(const std::string &url, const std::string &body, std::string &out) {
  if (!http_) return false;
  std::list<esphome::http_request::Header> hdr{
    {"Content-Type", "application/x-www-form-urlencoded"}
  };
  ESP_LOGI(TAG, "CALL_WS_ %s", url);
  return false;
  auto c = http_->post(url, body, hdr);
  if (!c) return false;

  std::string resp;
  uint8_t buf[256];
  for (;;) { int n = c->read(buf, sizeof(buf)); if (n <= 0) break; resp.append((char*)buf, n); }
  int code = c->status_code; c->end();

  ESP_LOGI(TAG, "HTTP %d, %u bytes", code, (unsigned)resp.size());
  if (!esphome::http_request::is_success(code)) return false;  // 2xx
  out.swap(resp);
  return true;
}

}  // namespace moodle_setup
}  // namespace esphome
