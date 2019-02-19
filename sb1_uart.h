#ifndef SB1_UART_H_
#define SB1_UART_H_

#include "esphome.h"
using namespace esphome;

#define SB1_MAX_LEN      256
#define SB1_BUFFER_LEN   6
#define SB1_HEADER_LEN   2
#define RESET_ACK_DELAY  250
#define MOTION_ACK_DELAY 500

static const char *TAG = "sb1";
static const uint16_t SB1_HEADER = 0x55AA;

static const uint8_t SB1_MOTION_ACK[]           = {0x00};
static const uint8_t SB1_STATUS_CONF_STA[]      = {0x00};
static const uint8_t SB1_STATUS_CONF_AP[]       = {0x01};
static const uint8_t SB1_STATUS_BOOT_WIFI[]     = {0x02};
static const uint8_t SB1_STATUS_BOOT_DHCP[]     = {0x03};
static const uint8_t SB1_STATUS_BOOT_COMPLETE[] = {0x04};

enum SB1MessageType {
  SB1_MESSAGE_TYPE_INVALID = 0,
  SB1_MESSAGE_TYPE_HANDSHAKE,
  SB1_MESSAGE_TYPE_STATUS,
  SB1_MESSAGE_TYPE_RESET,
  SB1_MESSAGE_TYPE_UNKNOWN,
  SB1_MESSAGE_TYPE_MOTION
};

enum SB1State {
  SB1_STATE_HANDSHAKE = 0,
  SB1_STATE_HANDSHAKE_ACK,
  SB1_STATE_CONF_AP,
  SB1_STATE_CONF_AP_ACK,
  SB1_STATE_CONF_STA,
  SB1_STATE_CONF_STA_ACK,
  SB1_STATE_BOOT_WIFI,
  SB1_STATE_BOOT_WIFI_ACK,
  SB1_STATE_BOOT_DHCP,
  SB1_STATE_BOOT_DHCP_ACK,
  SB1_STATE_BOOT_COMPLETE,
  SB1_STATE_BOOT_COMPLETE_ACK,
  SB1_STATE_RUNNING,
  SB1_STATE_MOTION_ACK,
  SB1_STATE_RESET_ACK
};

struct SB1Message {
  uint16_t header;
  uint16_t type;
  uint16_t length;
  uint8_t value[SB1_MAX_LEN];
  uint8_t checksum;
};

class SB1UARTComponent : public Component, public UARTDevice {
  protected:
    binary_sensor::BinarySensor *sensor_{nullptr};
    SB1State state_{SB1_STATE_HANDSHAKE};
    SB1Message message_{SB1_HEADER, SB1_MESSAGE_TYPE_INVALID, 0, {}, 0};
    ESPPreferenceObject rtc_;
    bool safe_mode_{false};
    uint32_t state_start_{0};
    uint8_t uart_buffer_[SB1_BUFFER_LEN]{0};

    bool read_message() {
      // Shift bytes through until we find a valid header
      bool valid_header = false;
      while (available() >= 1) {
        this->uart_buffer_[0] = this->uart_buffer_[1];
        read_byte(this->uart_buffer_ + 1);
        this->message_.header = (this->uart_buffer_[0] << 8) + this->uart_buffer_[1];
        if (this->message_.header == SB1_HEADER) {
          valid_header = true;
          break;
        }
      }

      // Read the next 4 bytes (type plus length), then the indicated length, then the checksum byte
      if (valid_header) {
        read_array(this->uart_buffer_ + SB1_HEADER_LEN, SB1_BUFFER_LEN - SB1_HEADER_LEN);
        this->message_.type = (this->uart_buffer_[2] << 8) + this->uart_buffer_[3];
        this->message_.length = (this->uart_buffer_[4] << 8) + this->uart_buffer_[5];
        ESP_LOGV(TAG, "Got message type=0x%04X length=0x%04X", this->message_.type, this->message_.length);

        if (this->message_.length <= SB1_MAX_LEN){
          read_array(this->message_.value, this->message_.length);
          read_byte(&this->message_.checksum);
          if (checksum() == this->message_.checksum) {
            // Clear buffer contents to start with beginning of next message
            memset(this->uart_buffer_, 0, SB1_BUFFER_LEN);
            return true;
          }
        } else {
          ESP_LOGE(TAG, "Message value too long: %d", this->message_.length);
        }
      }

      // Do not clear buffer to allow for resume in case of reading partway through header RX
      return false;
    }

    void write_message(SB1MessageType type, const uint8_t *value, uint16_t length) {
      // Copy params into message struct
      this->message_.header = SB1_HEADER;
      this->message_.type = type;
      this->message_.length = length;
      ESP_LOGV(TAG, "Sending message: header=0x%04X type=0x%04X length=0x%04X",
               this->message_.header, this->message_.type, this->message_.length);
      memcpy(&this->message_.value, value, length);
      // Copy struct values into buffer, converting ntohs()
      this->uart_buffer_[0] = this->message_.header >> 8;
      this->uart_buffer_[1] = this->message_.header & 0xFF;
      this->uart_buffer_[2] = this->message_.type >> 8;
      this->uart_buffer_[3] = this->message_.type & 0xFF;
      this->uart_buffer_[4] = this->message_.length >> 8;
      this->uart_buffer_[5] = this->message_.length & 0xFF;
      this->message_.checksum = checksum();
      // Send buffer out via UART
      write_array(this->uart_buffer_, SB1_BUFFER_LEN);
      write_array(this->message_.value, this->message_.length);
      write_byte(this->message_.checksum);
      // Clear buffer contents to avoid re-reading our own payload
      memset(this->uart_buffer_, 0, SB1_BUFFER_LEN);
    }

    /*
     * Calculate checksum from current UART buffer (header+type+length) plus message value
     */
    uint8_t checksum() {
      uint8_t checksum = 0;
      for (size_t i = 0; i < SB1_BUFFER_LEN; i++) {
        checksum += this->uart_buffer_[i];
      }
      for (size_t i = 0; i < this->message_.length; i++) {
        checksum += this->message_.value[i];
      }
      return checksum;
    }

    bool message_matches(SB1MessageType type, uint16_t length) {
      ESP_LOGV(TAG, "Checking type(%d == %d) && length(%d >= %d)",
               this->message_.type, type, this->message_.length, length);
      return (this->message_.type == type && this->message_.length >= length);
    }

    /* 
     * Update state machine
     */
    void set_state(SB1State state) {
      if (this->state_ != state){
        ESP_LOGD(TAG, "state: %d -> %d after %d ms", this->state_, state, state_duration());
        this->state_ = state;
        this->state_start_ = millis();
      }
    }

    /*
     * Get time since the last state change
     */
    uint32_t state_duration() {
      return (millis() - this->state_start_);
    }

    /*
     * Check to see if client connection is up - either
     * ESP as MQTT client to HA, or HA as API client to ESP.
     */
    bool client_is_connected() {
      // TODO: extend -core support to expose connected API client count
      if (mqtt::global_mqtt_client != nullptr && mqtt::global_mqtt_client->is_connected()){
        return true;
      } else {
        return false;
      }
    }

  public:
    SB1UARTComponent(UARTComponent *parent, binary_sensor::BinarySensor *sensor)
        : UARTDevice(parent)
        , sensor_(sensor) {}

    float get_setup_priority() const override {
      return setup_priority::HARDWARE_LATE;
    }

    void setup() override {
      this->rtc_ = global_preferences.make_preference<bool>(this->sensor_->get_object_id_hash());
      this->rtc_.load(&this->safe_mode_);
      ESP_LOGD(TAG, "Setting up SB1 UART; safe_mode = %d", this->safe_mode_);
    }

    // Don't do this in setup, otherwise we get called before the OTA component gets a chance
    // to clear the boot loop counter, due to our setup registering the hook first.
    // Hooks should probably be called in reverse order of registration.
    void add_late_hooks() {
      add_safe_shutdown_hook([this](const char *cause) {
        this->rtc_.save(&this->safe_mode_);
        ESP_LOGD(TAG, "SB1 UART shutting down; safe_mode = %d", this->safe_mode_);
        flush();

        if (this->state_ == SB1_STATE_MOTION_ACK) {
          write_message(SB1_MESSAGE_TYPE_MOTION, SB1_MOTION_ACK, 1);
        } else if (this->state_ == SB1_STATE_RESET_ACK) {
          write_message(SB1_MESSAGE_TYPE_RESET, nullptr, 0);
        }
      });
    }

    /* 
     * Giant ugly state machine
     */
    void loop() override {
      bool have_message = read_message();

      // Reboot events are user-initiated and can occur regardless of state
      if (have_message && message_matches(SB1_MESSAGE_TYPE_RESET, 0)) {
          set_state(SB1_STATE_RESET_ACK);
          return;
      }

      switch (this->state_) {
        case SB1_STATE_HANDSHAKE:
          write_message(SB1_MESSAGE_TYPE_HANDSHAKE, nullptr, 0);
          set_state(SB1_STATE_HANDSHAKE_ACK);
          break;
        case SB1_STATE_HANDSHAKE_ACK:
          if (have_message && message_matches(SB1_MESSAGE_TYPE_HANDSHAKE, 0)) {
            add_late_hooks();
            if (this->safe_mode_) {
              this->safe_mode_ = false;
              set_state(SB1_STATE_CONF_STA);
            } else { 
              set_state(SB1_STATE_BOOT_WIFI);
            }
          } else if (state_duration() > 1000) {
            set_state(SB1_STATE_HANDSHAKE);
          }
          break;
        case SB1_STATE_CONF_AP:
          write_message(SB1_MESSAGE_TYPE_STATUS, SB1_STATUS_CONF_AP, 1);
          set_state(SB1_STATE_CONF_AP_ACK);
          break;
        case SB1_STATE_CONF_STA:
          write_message(SB1_MESSAGE_TYPE_STATUS, SB1_STATUS_CONF_STA, 1);
          set_state(SB1_STATE_CONF_STA_ACK);
          break;
        case SB1_STATE_BOOT_WIFI:
          if (global_wifi_component->is_connected()) {
            write_message(SB1_MESSAGE_TYPE_STATUS, SB1_STATUS_BOOT_WIFI, 1);
            set_state(SB1_STATE_BOOT_WIFI_ACK);
          }
          break;
        case SB1_STATE_BOOT_WIFI_ACK:
          if (have_message && message_matches(SB1_MESSAGE_TYPE_STATUS, 0)) {
            set_state(SB1_STATE_BOOT_DHCP);
          }
          break;
        case SB1_STATE_BOOT_DHCP:
          if (global_wifi_component->get_ip_address() != (uint32_t)0 ) {
            write_message(SB1_MESSAGE_TYPE_STATUS, SB1_STATUS_BOOT_DHCP, 1);
            set_state(SB1_STATE_BOOT_DHCP_ACK);
          }
          break;
        case SB1_STATE_BOOT_DHCP_ACK:
          if (have_message && message_matches(SB1_MESSAGE_TYPE_STATUS, 0)) {
            set_state(SB1_STATE_BOOT_COMPLETE);
          }
          break;
        case SB1_STATE_BOOT_COMPLETE:
          if (client_is_connected()) {
            write_message(SB1_MESSAGE_TYPE_STATUS, SB1_STATUS_BOOT_COMPLETE, 1);
            set_state(SB1_STATE_BOOT_COMPLETE_ACK);
          }
          break;
        case SB1_STATE_BOOT_COMPLETE_ACK:
        case SB1_STATE_CONF_AP_ACK:
        case SB1_STATE_CONF_STA_ACK:
          if (have_message && message_matches(SB1_MESSAGE_TYPE_STATUS, 0)) {
            set_state(SB1_STATE_RUNNING);
          }
          break;
        case SB1_STATE_RUNNING:
          if (have_message && message_matches(SB1_MESSAGE_TYPE_MOTION, 0)) {
            set_state(SB1_STATE_MOTION_ACK);
            // TODO - there's some sort of payload in this message; not sure what it is.
            ESP_LOGD(TAG, "Motion event:");
            for (size_t i = 0; i < this->message_.length; i++) {
              ESP_LOGD(TAG, "%02d: 0x%02X", i, this->message_.value[i]);
            }
            if (this->sensor_ != nullptr) {
              this->sensor_->publish_state(true);
            }
          }
          break;
        case SB1_STATE_MOTION_ACK:
          if (state_duration() > MOTION_ACK_DELAY) {
            this->safe_mode_ = false;
            safe_reboot("sb1-motion");
          }
          break;
        case SB1_STATE_RESET_ACK:
          if (state_duration() > RESET_ACK_DELAY) {
            this->safe_mode_ = true;
            safe_reboot("sb1-reset");
          }
          break;
      }
    }

};

#endif // SB1_UART_H_
