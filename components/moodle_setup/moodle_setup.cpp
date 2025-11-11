#include "moodle_setup.h"
#include "esphome/core/log.h"
#include "esphome/core/time.h"
#include "esphome/components/time/real_time_clock.h"
#include <ESPAsyncWebServer.h>
//#include <AsyncTCP.h>

#include <rapidjson/error/en.h>
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

// Abifunktsioon paari jaoks: key=value (mõlemad kodeeritud)
static inline std::string form_pair(const std::string &k, const std::string &v) {
  return url_encode(k) + "=" + url_encode(v);
}


// mem_report.h

#include <stdint.h>
#include <stddef.h>

struct HeapSnapshot {
  size_t free_bytes = 0;        // total free heap
  size_t largest_block = 0;     // largest allocatable block
  uint8_t fragmentation = 0;    // % (ESP8266 only, else derived)
};

inline HeapSnapshot take_heap_snapshot() {
  HeapSnapshot s{};
#if defined(ARDUINO_ARCH_ESP8266)
  // <Arduino.h> already included by ESPHome; ESP.* is available
  s.free_bytes     = ESP.getFreeHeap();
  s.largest_block  = ESP.getMaxFreeBlockSize();
  s.fragmentation  = ESP.getHeapFragmentation();  // 0..100
#elif defined(ARDUINO_ARCH_ESP32)
  // FreeRTOS/IDF heap APIs
  #include "esp_heap_caps.h"
  s.free_bytes     = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  s.largest_block  = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  // crude fragmentation estimate: 100 * (1 - largest/free)
  s.fragmentation  = (s.free_bytes > 0)
                     ? (uint8_t)(100 - (100.0 * (double)s.largest_block / (double)s.free_bytes))
                     : 0;
#else
  // Fallback if ported elsewhere
  s.free_bytes = s.largest_block = 0;
  s.fragmentation = 0;
#endif
  return s;
}



#undef RAPIDJSON_PARSE_DEFAULT_FLAGS
#define RAPIDJSON_PARSE_DEFAULT_FLAGS ::rapidjson::kParseNoFlags

#include <rapidjson/reader.h>
#include <string>
#include <vector>
using namespace rapidjson;


struct EventMinimal {
  uint32_t id = 0, timesort = 0;
  std::string name, component, modulename;
};


class ChunkQueue {
 public:
  void push(const char* data, size_t n) {
    if (n) { chunks_.emplace_back(data, n); total_size_ += n; }
    ESP_LOGI("moodle_setup", "ADDING chunk now: %d", total_size_);
  }
  void mark_eof() { eof_ = true; }
  bool eof() const { return eof_ && total_size_ == 0; }

  // NEW: drop already-consumed chunk(s) at the front
  void drop_empty_front_() {
    while (!chunks_.empty() && chunks_.front().second == 0) {
      ESP_LOGI("moodle_setup", "POP chunk");
      chunks_.pop_front();
    }
  }
  
  char peek() {
    drop_empty_front_();
    if (chunks_.empty()) return '\0';
    return chunks_.front().first[0];
  }

  char take() {
    drop_empty_front_();
    if (chunks_.empty()) return '\0';
    auto& front = chunks_.front();
    char c = front.first[0];
    //ESP_LOGI("moodle_setup", "TAKE %d", c);
    front.first.remove_prefix(1);
    front.second -= 1;
    total_size_ -= 1;
    // don’t pop here; next call will drop it via drop_empty_front_()
    return c;
  }

  bool clear() {
    chunks_.clear();
    total_size_ = 0;
    eof_ = false;
    return true;
  }

 private:
  struct View {
    std::string storage;
    std::string_view first;
    size_t second;
    View(const char* p, size_t n) : storage(p, n), first(storage.data(), n), second(n) {}
  };
  std::deque<View> chunks_;
  size_t total_size_ = 0;
  bool eof_ = false;
};



#include <rapidjson/reader.h>

 struct ChunkedInputStream {
   using Ch = char;
   explicit ChunkedInputStream(ChunkQueue& q) : q_(q), tell_(0) {}
   Ch Peek() {                // called a lot; must be fast
     last_ = q_.peek();
     return last_;
   }
   Ch Take() {                // advances one byte
     Ch c = q_.take();
     if (c != '\0') ++tell_;
     return c;
   }
   size_t Tell() const { return tell_; }

   // Unused by non-insitu parsing:
   Ch*   PutBegin() { return nullptr; }
   void  Put(Ch) {}
   size_t PutEnd(Ch*) { return 0; }

   void Reset() { tell_ = 0; }
 private:
   ChunkQueue& q_;
   size_t tell_;
   Ch last_{0};
 };



 #include <rapidjson/reader.h>
using namespace rapidjson;

struct MinimalEvent {
  uint64_t id = 0;
  bool overdue = false;
  bool hasId = false;
  std::string name; 
  bool hasOverdue = false;
  void reset(){ id=0; overdue=false; hasId=false; hasOverdue=false; }
};

struct EventsHandler : BaseReaderHandler<UTF8<>, EventsHandler> {
  bool inEvents = false, inEventObj = false;
  std::string curKey;
  MinimalEvent ev;

  // top-level key detection
  bool Key(const char* s, SizeType n, bool) {
    // if (!inEventObj) curKey.assign(s, n);
    curKey.assign(s, n);
    ESP_LOGI("moodle", "event KEY %s", curKey.c_str());
    return true;
  }
  bool StartArray() {
    ESP_LOGI("moodle", "STARTARRAY!!!!!!");
    if (curKey == "events" && !inEvents) {
      ESP_LOGI("moodle", "TURNING inEvents ON");
      inEvents = true;
    }
    return true;
  }
  bool EndArray(SizeType){ ESP_LOGI("moodle", "ENDARRAY!!!!!!"); if (inEvents) inEvents = false; return true; }

  bool StartObject() {     ESP_LOGI("moodle", "STARTOBJECT!!!!!!"); if (inEvents) { inEventObj = true; ev.reset(); } return true; }
  bool EndObject(SizeType) {
    ESP_LOGI("moodle", "ENDOBJECT!!!!!!");
    if (inEventObj) {
      ESP_LOGI("moodle", "inEventObj %d", ev.id);
      if (ev.hasId) {
        handle_event(ev.id, ev.overdue);
      }
      inEventObj = false;
    }
    return true;
  }

  // values inside event objects
  bool KeyInEvent(const char* s, SizeType n){     ESP_LOGI("moodle", "KEYINEVENT!!!!!!"); curKey.assign(s, n); return true; }
  bool Uint(unsigned v){ ESP_LOGI("moodle", "UINT!!!!! curKey: %s, %d", curKey.c_str(), v); if (inEventObj && curKey=="id") {  ev.id=v; ev.hasId=true; } return true; }
  bool Uint64(uint64_t v){    ESP_LOGI("moodle", "UINT64!!!!!!"); if (inEventObj && curKey=="id"){ ev.id=v; ev.hasId=true; } return true; }
  bool Int(int v){    ESP_LOGI("moodle", "INT!!!!!!"); if (inEventObj && curKey=="id"){ ev.id=(uint64_t)v; ev.hasId=true; } return true; }
  bool Bool(bool b){    ESP_LOGI("moodle", "BOOL!!!!!!"); if (inEventObj && curKey=="overdue"){ ev.overdue=b; ev.hasOverdue=true; } return true; }

  // ignore everything else
  bool Null(){ ESP_LOGI("moodle", "NULL!!!!!!"); return true; }
  bool Double(double){ ESP_LOGI("moodle", "DOUBLE!!!!!!"); return true; }
  bool Int64(int64_t v){ ESP_LOGI("moodle", "Int64!!!!!!"); return Int((int)v); }
  bool String(const char* c, SizeType s, bool b)
  {
    if (curKey == "name" && inEventObj) {
      ev.name.assign(c, s);
      ESP_LOGI("moodle_setup", "String name %s", ev.name.c_str());
    }
    return true;
  } 
  bool RawNumber(const char*, SizeType, bool){ ESP_LOGI("moodle", "RawNumber!!!!!!"); return true; }

  // hook to your code
  static void handle_event(uint64_t id, bool overdue) {
    // TODO: your lightweight action here (don’t allocate big strings)
    ESP_LOGI("moodle", "HANDLE_EVENT!!!!!!");
    //    ESP_LOGI("moodle", "event id=%llu overdue=%s", (unsigned long long)id, overdue ? "true":"false");
  }
};



 #include <rapidjson/reader.h>
using namespace rapidjson;

class EventsStreamParser {
 public:
  EventsStreamParser() : stream_(queue_) {
    // Force non-insitu parsing even if project defines defaults elsewhere
    reader_.IterativeParseInit();
  }

  // Call this for each incoming chunk. Set eof=true for the last chunk.
  // Returns true when a full JSON document is completely parsed.
  bool feed(const char* data, size_t n, bool eof=false) {
    if (reader_.HasParseError()) {
      auto code = reader_.GetParseErrorCode();
      auto off  = reader_.GetErrorOffset();

      ESP_LOGE("moodle", "WE STILL HAVE JSON parse error: %d at offset %u",
               (int)code, (unsigned)off);
      // error_ = true;
      // return false;
    }
    
    ESP_LOGE("moodle", "eof %d", eof);


    // ESP_LOGI("moodle", "feed %d", n);
    if (n) queue_.push(data, n);
    if (eof) queue_.mark_eof();
    const bool may_have_more = !eof;
    // Pump the parser as far as we can with the bytes we have.
    while (reader_.IterativeParseNextNonFatal<kParseStopWhenDoneFlag>(stream_, handler_, may_have_more)) {
      // keep going until RapidJSON needs more input or is done
      //      ESP_LOGI("moodle", "keep going until RapidJJson in done");
    }
    ESP_LOGI("moodle", "while loop END");
    if (reader_.IterativeParseComplete()) {
      ESP_LOGI("moodle", "END feed parse complete");
      return true; // finished cleanly
    }

    if (reader_.HasParseError()) {
      auto code = reader_.GetParseErrorCode();
      auto off  = reader_.GetErrorOffset();

      // If not real EOF yet, treat "termination-like" errors as "need more data"
      if (!queue_.eof()) {
        // just wait for more bytes; DO NOT mark error_
        return false;
      }

      ESP_LOGE("moodle", "JSON parse error: %d at offset %u",
               (int)code, (unsigned)off);
      error_ = true;
      return false;
    }

    // ESP_LOGI("moodle", "while loop");
    if (reader_.HasParseError()) {
      // If we reached EOF and still got an error -> real parse error.
      ESP_LOGE("moodle", "JSON parse error");
      if (queue_.eof()) {
        auto code = reader_.GetParseErrorCode();
        // Optional: log or map code to message
        ESP_LOGE("moodle", "JSON parse error: %d at offset %u", (int)code, (unsigned)reader_.GetErrorOffset());
        error_ = true;
      }
      // else: parser just needs more data; ignore for now
    }
    // ESP_LOGI("moodle", "while return");
    return false;
  }

  bool has_error() const { return error_; }
  bool clear() { stream_.Reset(); queue_.clear(); reader_.IterativeParseInit(); return true; }

 private:
  ChunkQueue queue_;
  ChunkedInputStream stream_;
  Reader reader_;
  EventsHandler handler_;
  bool error_ = false;
};

EventsStreamParser parser;




namespace esphome {
namespace moodle_setup {


static const char *const TAG = "moodle_setup";

  // using web_server::global_web_server;
  MoodleSetup::MoodleSetup()  { }

  MoodleSetup::MoodleSetup(web_server_base::WebServerBase *base) : base_(base) { }

  void MoodleSetup::dump_config() {
    ESP_LOGCONFIG(TAG, "user: %s", this->user.c_str());
    ESP_LOGCONFIG(TAG, "token: %s", this->token.c_str());
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
  if (this->state_ == MOODLE_STATE_LOGIN) {
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
      // save token
      this->save_to_prefs_();
      this->state_ = MOODLE_STATE_TOKEN_RECEIVED;
      this->conn_->end();

    }
    // buf[9] = '\0';

    // ESP_LOGI(TAG, "resp code %d", buf);
    // return;
  }
  if (this->state_ == MOODLE_STATE_START_LOGIN) {
    ESP_LOGI(TAG, "Loop starts login");

    std::string body = "username=" + url_encode(this->user)
      + "&password=" + url_encode(this->password)
      + "&service="  + url_encode("moodle_mobile_app"); // või sinu teenus

    ESP_LOGI(TAG, "user: %s", this->user.c_str());
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
    this->state_ = MOODLE_STATE_LOGIN;
    return;
  }

  if (this->state_ == MOODLE_STATE_START_REQUESTING_TASKS) {
    ESP_LOGI(TAG, "Loop starts requesting tasks.");

    uint32_t now;
    if (!this->time_) {
      ESP_LOGI(TAG, "time_ NULL");
    }

    auto t = this->time_ ? this->time_->now() : esphome::ESPTime{};
    if (t.is_valid()) {
      uint32_t now = static_cast<uint32_t>(t.timestamp);  // epoch seconds
      // use 'now' in your Moodle request
      ESP_LOGI(TAG, "t.timestamp : %d, now %d", t.timestamp, now);
    } else {
      // not synced yet → choose a fallback
      uint32_t now = 1759320000;  // or keep a wide window
      ESP_LOGI(TAG, "Time FAILED");
    }

    const uint32_t horizon = t.timestamp + 120 * 24 * 3600;

   std::string body =
    form_pair("wstoken", this->token) + "&" +
    form_pair("wsfunction", "core_calendar_get_action_events_by_timesort") + "&" +
    form_pair("moodlewsrestformat", "json") + "&" +
    form_pair("timesortfrom", std::to_string(t.timestamp)) + "&" +
    form_pair("timesortto", std::to_string(horizon)) + "&" +
    form_pair("limitnum", "1");

   ESP_LOGI(TAG, "BODY: %s", body.c_str());
   std::list<esphome::http_request::Header> hdr{
     {"Content-Type", "application/x-www-form-urlencoded"}
   };

   parser.clear();
   auto c = http_->get("http://192.168.0.100:7788/events-one-clean.json");

   // auto c = http_->post("https://moodle.tlu.ee/webservice/rest/server.php", body, hdr);

   this->conn_ = c;
    ESP_LOGI(TAG, "REST HTTP status: %d, CL=%d, read=%u", c->status_code, c->content_length, (unsigned)c->get_bytes_read());
   this->state_ = MOODLE_STATE_REQUESTING_TASKS;

   return;
  }
  if (this->state_ == MOODLE_STATE_REQUESTING_TASKS) {
    uint8_t buf[100];
    auto s = take_heap_snapshot();
    ESP_LOGI(TAG, "[tick] free=%u, largest=%u, frag=%u%%",
             (unsigned)s.free_bytes, (unsigned)s.largest_block, (unsigned)s.fragmentation);
    int n = this->conn_->read(buf, 100);
    ESP_LOGI(TAG, "tasks incoming n %d", n);
    if (n < 0) {
      ESP_LOGI(TAG, "TASKS END");
      this->state_ = MOODLE_STATE_IDLE;
    }
    //json.append(reinterpret_cast<const char *>(buf), static_cast<size_t>(n));
    parser.feed((const char *)buf, n);
    s = take_heap_snapshot();
    ESP_LOGI(TAG, "[tick2] free=%u, largest=%u, frag=%u%%",
             (unsigned)s.free_bytes, (unsigned)s.largest_block, (unsigned)s.fragmentation);

    buf[n] = '\0';
    ESP_LOGI(TAG, "buf: %s", buf);
    ESP_LOGI(TAG, "REQUESTING TASKS status: %d, CL=%d, read=%u", this->conn_->status_code, this->conn_->content_length, (unsigned)this->conn_->get_bytes_read());
    if (n == 0) {
      this->state_ = MOODLE_STATE_IDLE;
      ESP_LOGI(TAG, "Closing connection");
      this->conn_->end();
      
      return;
    }

    // Create once; set 'chunked' true if server uses Transfer-Encoding: chunked

  
    

    if (this->conn_->content_length > 0 && this->conn_->get_bytes_read() >= this->conn_->content_length) {
      ESP_LOGI(TAG, "pump OFF");
      // save tasks
      //this->save_to_prefs_();
      this->state_ = MOODLE_STATE_IDLE;
      this->conn_->end();
      //parser.feed(NULL, 0, true);

    }
    // buf[9] = '\0';

    // ESP_LOGI(TAG, "resp code %d", buf);
    // return;
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
                    "<input type='password' name='password' value=''></label>"
                    "<p><em>Soovitus: kasuta parooli asemel web-service tokenit.</em></p>"
                    "<label>Moodle token<br>"
                                  "<input name='token' value=''></label>"));

    stream->print(this->token.c_str());
    stream->print(F("<br><br><button type='submit'>Salvesta</button>"
                    "</form></br>"));
    stream->print(F("<form method='POST' action='/moodle/start'>"                     
                    "<button type='submit'>Login</button>"
                    "</form><form method='POST' action='/moodle/request_tasks'>"                     
                    "<button type='submit'>Request tasks</button>"
                    "</form></code></p></body></html>"));


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
        if (p->name() == "password") {
            ESP_LOGI(TAG, "[%d] name='%s'  isPost=%d isFile=%d", i, p->name().c_str(), p->isPost(), p->isFile());
            continue;
          }
        else {
          ESP_LOGI(TAG, "[%d] name='%s' value='%s' isPost=%d isFile=%d",
                   i, p->name().c_str(), p->value().c_str(), p->isPost(), p->isFile());
        }
      }
    const String user_s = get_param_(req, "user");
    ESP_LOGI(TAG, "user %s", req->arg("user"));   

    const String password_s = get_param_(req, "password");
    const String token_s = get_param_(req, "token");

    // Uuenda RAM-is olevaid väärtusi (parooli ei kirjuta üle tühjaga)
    if (user_s.length() > 0) this->user = std::string(user_s.c_str());
    if (password_s.length() > 0) this->password = std::string(password_s.c_str());
    if (token_s.length() > 0) this->token = std::string(token_s.c_str());

    // Kirjuta püsisalvestusse
    this->save_to_prefs_();

    String resp;
    resp += F("<!doctype html><html><meta charset='utf-8'>"
              "<body><h3>Salvestatud.</h3>"
              "<p>Tagasi <a href='/moodle'>Moodle seadistusse</a>.</p>"
              "</body></html>");
    req->send(200, "text/html; charset=utf-8", resp);
    ESP_LOGI(TAG, "user %s", user_s.c_str());   
    ESP_LOGI(TAG, "Starting connection to Moodle.");

    }

   if (req->url() == F("/moodle/start")) {
     ESP_LOGI(TAG, "POST /moodle/start.");
     this->state_ = MOODLE_STATE_START_LOGIN;
     return;
   }

   if (req->url() == F("/moodle/request_tasks")) {
     ESP_LOGI(TAG, "POST /moodle/request_tasks.");
     this->state_ = MOODLE_STATE_START_REQUESTING_TASKS;
     return;
   }

   ESP_LOGI(TAG, "Endpoints ready: GET /moodle, POST /moodle/save, POST /moodle/start, POST /moodle/request_tasks");

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
    this->user = save.user;
    this->password = save.password;
    this->token = save.token;
  }
  else {
    ESP_LOGD(TAG, "Failed to load settings.");
  }
}

void MoodleSetup::save_to_prefs_() {
  MoodleSettings save{};
  snprintf(save.user, sizeof(save.user), "%s", this->user.c_str());
  snprintf(save.password, sizeof(save.password), "%s", this->password.c_str());
  snprintf(save.token, sizeof(save.token), "%s", this->token.c_str());
  ESP_LOGD(TAG, "save_to_prefs user: %s", save.user);
  ESP_LOGD(TAG, "save_to_prefs token: %s", save.token);
  this->pref_.save(&save);
  // ensure it's written immediately
  global_preferences->sync();

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



}  // namespace moodle_setup
}  // namespace esphome
