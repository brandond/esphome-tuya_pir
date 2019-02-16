#ifndef SB1_UART_H_
#define SB1_UART_H_

#include "esphome.h"
using namespace esphome;

enum SB1State {
  SB1_STATE_HANDSHAKE = 0,
  SB1_STATE_HANDSHAKE_ACK,
  SB1_STATE_AP,
  SB1_STATE_AP_ACK,
  SB1_STATE_STA,
  SB1_STATE_STA_ACK,
  SB1_STATE_WIFI,
  SB1_STATE_WIFI_ACK,
  SB1_STATE_IP,
  SB1_STATE_IP_ACK,
  SB1_STATE_MQTT,
  SB1_STATE_MQTT_ACK,
  SB1_STATE_RUNNING,
  SB1_STATE_MOTION_ACK,
  SB1_STATE_RESET_ACK
};

static const char *TAG = "sb1";

static const uint8_t SB1_HEADER[]        = {0x55, 0xAA};

static const uint8_t ESP_HANDSHAKE_REQ[] = {0x55, 0xAA, 0x00, 0x01, 0x00, 0x00, 0x00};
static const uint8_t SB1_HANDSHAKE_ACK[] = {0x55, 0xAA, 0x00, 0x01, 0x00};
static const uint8_t SB1_HANDSHAKE_END[] = {0x19};

static const uint8_t ESP_STATUS_STA[]    = {0x55, 0xAA, 0x00, 0x02, 0x00, 0x01, 0x00, 0x02};
static const uint8_t ESP_STATUS_AP[]     = {0x55, 0xAA, 0x00, 0x02, 0x00, 0x01, 0x01, 0x03};
static const uint8_t ESP_STATUS_WIFI[]   = {0x55, 0xAA, 0x00, 0x02, 0x00, 0x01, 0x02, 0x04};
static const uint8_t ESP_STATUS_IP[]     = {0x55, 0xAA, 0x00, 0x02, 0x00, 0x01, 0x03, 0x05};
static const uint8_t ESP_STATUS_MQTT[]   = {0x55, 0xAA, 0x00, 0x02, 0x00, 0x01, 0x04, 0x06};
static const uint8_t SB1_STATUS_ACK[]    = {0x55, 0xAA, 0x00, 0x02, 0x00, 0x00, 0x01};

static const uint8_t SB1_RESET_EVT[]     = {0x55, 0xAA, 0x00, 0x03, 0x00, 0x00, 0x02};
#define              ESP_RESET_ACK         SB1_RESET_EVT

static const uint8_t SB1_MOTION_EVT[]    = {0x55, 0xAA, 0x00, 0x05, 0x00, 0x05, 0x65, 0x01, 0x00, 0x01, 0x00, 0x70};
static const uint8_t ESP_MOTION_ACK[]    = {0x55, 0xAA, 0x00, 0x05, 0x00, 0x01, 0x00, 0x05};
#define              SB1_MESSAGE_MAX       64
#define              SB1_HEADER_LEN        2
#define              LOOP_DIVISOR          10


class SB1UARTComponent : public Component, public UARTDevice {
  protected:
    SB1State state_{SB1_STATE_HANDSHAKE};
    uint8_t sb1_in_[SB1_MESSAGE_MAX]{};
    uint8_t sb1_bufpos_{0};
    uint32_t state_start_{0};
    uint8_t loop_counter_{0};

    bool check_buffer(const uint8_t *message, size_t len) {
      fill_buffer();
      if (memcmp(this->sb1_in_, message, len) == 0) {
        trim_buffer(len);
        return true;
      } else {
        return false;
      }
    }

    void fill_buffer(bool trim = true){
      while (this->sb1_bufpos_ < SB1_MESSAGE_MAX &&
             available()){
        read_byte(this->sb1_in_ + this->sb1_bufpos_++);
      }

      while(trim &&
            this->sb1_bufpos_ >= SB1_HEADER_LEN &&
            memcmp(this->sb1_in_, SB1_HEADER, SB1_HEADER_LEN) != 0){
        trim_buffer(SB1_HEADER_LEN);
      }
    }

    void trim_buffer(size_t len) {
      if (len > this->sb1_bufpos_) {
        len = this->sb1_bufpos_;
      }
      size_t remainder = this->sb1_bufpos_ - len;
      ESP_LOGD(TAG, "Trimming %d of %d bytes", len, this->sb1_bufpos_);
      memmove(this->sb1_in_, this->sb1_in_ + len, remainder);
      memset(this->sb1_in_ + remainder, 0x00, SB1_MESSAGE_MAX - remainder);
      this->sb1_bufpos_ = remainder;
      ESP_LOGD(TAG, "%d bytes left", this->sb1_bufpos_);
    }

    void trim_until(const uint8_t end) {
      uint32_t start_time = millis();
      uint8_t *pchr;
      ESP_LOGD(TAG, "Trim until %#x", end);
      while (true){
        pchr = (uint8_t*) memchr(this->sb1_in_, end, this->sb1_bufpos_);
        if (pchr == nullptr) {
          ESP_LOGD(TAG, "Clearing %d bytes", this->sb1_bufpos_);
          if (millis() - start_time > 100) {
            break;
          } else {
            yield();
            this->sb1_bufpos_ = 0;
            memset(this->sb1_in_, 0x00, SB1_MESSAGE_MAX);
            fill_buffer(false);
          }
        } else {
          ESP_LOGD(TAG, "End marker found");
          trim_buffer(pchr - this->sb1_in_ + 1);
          break;
        }
      }
    }

    void set_state(SB1State state) {
      ESP_LOGD(TAG, "state: %d -> %d after %d ms", this->state_, state, state_duration());
      this->state_ = state;
      this->state_start_ = millis();
    }

    uint32_t state_duration() {
      return (millis() - this->state_start_);
    }

    bool loop_divisor() {
      if (this->loop_counter_++ == LOOP_DIVISOR) {
        this->loop_counter_ = 0;
        return true;
      } else {
        return false;
      }
    }

  public:
    SB1UARTComponent(UARTComponent *parent) : UARTDevice(parent) {}

    float get_setup_priority() const override {
      return setup_priority::HARDWARE_LATE;
    }

    void loop() override {
      if (! loop_divisor()){
        return;
      }

      switch (this->state_) {
        case SB1_STATE_HANDSHAKE:
          write_array(ESP_HANDSHAKE_REQ, sizeof(ESP_HANDSHAKE_REQ));
          set_state(SB1_STATE_HANDSHAKE_ACK);
          break;
        case SB1_STATE_HANDSHAKE_ACK:
          if (check_buffer(SB1_HANDSHAKE_ACK, sizeof(SB1_HANDSHAKE_ACK))) {
            trim_until(SB1_HANDSHAKE_END[0]);
            if (global_wifi_component->has_ap()) {
              set_state(SB1_STATE_AP);
            } else if (global_wifi_component->has_sta()) {
              set_state(SB1_STATE_WIFI);
            } else {
              set_state(SB1_STATE_STA);
            }
          } else {
            if (state_duration() > 1000) {
              set_state(SB1_STATE_HANDSHAKE);
            }
          }
          break;
        case SB1_STATE_AP:
          write_array(ESP_STATUS_AP, sizeof(ESP_STATUS_AP));
          set_state(SB1_STATE_AP_ACK);
          break;
        case SB1_STATE_STA:
          write_array(ESP_STATUS_STA, sizeof(ESP_STATUS_STA));
          set_state(SB1_STATE_STA_ACK);
          break;
        case SB1_STATE_WIFI:
          if (global_wifi_component->is_connected()) {
            write_array(ESP_STATUS_WIFI, sizeof(ESP_STATUS_WIFI));
            set_state(SB1_STATE_WIFI_ACK);
          }
          break;
        case SB1_STATE_WIFI_ACK:
          if (check_buffer(SB1_STATUS_ACK, sizeof(SB1_STATUS_ACK))) {
            set_state(SB1_STATE_IP);
          }
          break;
        case SB1_STATE_IP:
          if (global_wifi_component->get_ip_address() != (uint32_t)0 ) {
            write_array(ESP_STATUS_IP, sizeof(ESP_STATUS_IP));
            set_state(SB1_STATE_IP_ACK);
          }
          break;
        case SB1_STATE_IP_ACK:
          if (check_buffer(SB1_STATUS_ACK, sizeof(SB1_STATUS_ACK))) {
            set_state(SB1_STATE_MQTT);
          }
          break;
        case SB1_STATE_MQTT:
          if (mqtt::global_mqtt_client->is_connected()) {
            write_array(ESP_STATUS_MQTT, sizeof(ESP_STATUS_MQTT));
            set_state(SB1_STATE_MQTT_ACK);
          }
          break;
        case SB1_STATE_MQTT_ACK:
        case SB1_STATE_AP_ACK:
        case SB1_STATE_STA_ACK:
          if (check_buffer(SB1_STATUS_ACK, sizeof(SB1_STATUS_ACK))) {
            set_state(SB1_STATE_RUNNING);
          }
          break;
        case SB1_STATE_RUNNING:
          if (check_buffer(SB1_RESET_EVT, sizeof(SB1_RESET_EVT))) {
            // ack then reboot after delay; SB1 doesn't actually care if we reset or not.
            write_array(ESP_RESET_ACK, sizeof(ESP_RESET_ACK));
            set_state(SB1_STATE_RESET_ACK);
          } else if (check_buffer(SB1_MOTION_EVT, sizeof(SB1_MOTION_EVT))) {
            // send message then ack after delay; SB1 cuts power as soon as we ack.
            set_state(SB1_STATE_MOTION_ACK);
          } else if (this->sb1_bufpos_ > 0) {
            ESP_LOGD(TAG, "Unhandled event with %d bytes", this->sb1_bufpos_);
            for (size_t i = 0; i < this->sb1_bufpos_; i++){
              ESP_LOGD(TAG, "Byte %d: %#x", i, sb1_in_[i]);
            }
          }
          break;
        case SB1_STATE_MOTION_ACK:
          if (state_duration() > 3000) {
            write_array(ESP_MOTION_ACK, sizeof(ESP_MOTION_ACK));
            set_state(SB1_STATE_RUNNING);
          }
          break;
        case SB1_STATE_RESET_ACK:
          if(state_duration() > 500) {
            reboot("sb1");
            set_state(SB1_STATE_RUNNING);
            break;
          }
      }
    }

};

#endif // SB1_UART_H_
