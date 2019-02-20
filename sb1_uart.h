#ifndef SB1_UART_H_
#define SB1_UART_H_

#include "esphome.h"
using namespace esphome;

#define SB1_MAX_LEN      256   // Max length of message value
#define SB1_BUFFER_LEN   6     // Length of serial buffer for header + type + length
#define SB1_HEADER_LEN   2     // Length of fixed header
#define RESET_ACK_DELAY  250   // Time to wait before rebooting due to reset
#define MOTION_ACK_DELAY 500   // Time to wait before acking motion event and getting put to sleep
#define ACK_WAIT_TIMEOUT 1000  // Time to wait for handshake response before re-sending request
#define OTA_REBOOT_DELAY 30000 // Time to stay in 'running' state waiting for OTA before rebooting
                               // This is a safety measure; although the SB1 will let us stay up for about 120
                               // seconds, starting an OTA too late will likely result in the ESP getting put
                               // to sleep before eboot can finish copying the new image. Since eboot clears
                               // the copy command BEFORE copying the OTA image over the permanent image,
                               // this can leave the device in an unbootable state.

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
    bool hooks_added_{false};
    uint32_t state_start_{0};
    uint8_t uart_buffer_[SB1_BUFFER_LEN]{0};
    uint8_t product_info_[SB1_MAX_LEN]{0};

    /*
     * Attempt to read an entire message from the serial UART into the message struct.
     * Will fail early if unable to find the two-byte header in the current
     * data stream. If the header is found, it will contine to read the complete
     * TLV+checksum sequence off the port. If the entire sequence can be read
     * and the checksum is valid, it will return true.
     */
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

        if (this->message_.length < SB1_MAX_LEN){
          read_array(this->message_.value, this->message_.length);
          read_byte(&this->message_.checksum);
          if (checksum() == this->message_.checksum) {
            // Clear buffer contents to start with beginning of next message
            memset(this->uart_buffer_, 0, SB1_BUFFER_LEN);
            return true;
          }
        } else {
          memset(this->uart_buffer_, 0, SB1_BUFFER_LEN);
          ESP_LOGE(TAG, "Message length exceeds limit: %d >= %d", this->message_.length, SB1_MAX_LEN);
        }
      }

      // Do not clear buffer to allow for resume in case of reading partway through header RX
      return false;
    }

    /*
     * Store the given type, value, and length into the message struct and send
     * it out the serial port. Automatically calculates the checksum as well.
     */
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
     * Calculate checksum from current UART buffer (header+type+length) plus message value.
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

    /*
     * Add safe shutdown hooks after the main loop has started.
     *
     * Don't do this in setup, otherwise we get called before the OTA component gets a chance
     * to clear the boot loop counter, due to our setup registering the hook first.
     * Hooks should probably be called in reverse order of registration.
     */
    void add_late_hooks() {
      ESP_LOGV(TAG, "Adding safe shutdown hooks...");
      add_safe_shutdown_hook([this](const char *cause) {
        this->rtc_.save(&this->safe_mode_);
        ESP_LOGD(TAG, "SB1 UART shutting down; safe_mode = %d", this->safe_mode_);
        flush();

        switch (this->state_) {
          case SB1_STATE_MOTION_ACK:
            write_message(SB1_MESSAGE_TYPE_MOTION, SB1_MOTION_ACK, 1);
            break;
          case SB1_STATE_RESET_ACK:
          case SB1_STATE_RUNNING:
            write_message(SB1_MESSAGE_TYPE_RESET, nullptr, 0);
            break;
          default:
            break;
        }
      });
    }

  public:
    SB1UARTComponent(UARTComponent *parent, binary_sensor::BinarySensor *sensor)
        : UARTDevice(parent)
        , sensor_(sensor) {}

    // Run after hardware (UART), but before WiFi and MQTT so that we can
    // send status messages to the SB1 as these components come up.
    float get_setup_priority() const override {
      return setup_priority::HARDWARE_LATE;
    }

    // Run very late in the loop, so that other components can process
    // before we check on their state and send status to the SB1.
    float get_loop_priority() const override {
      return -10.0f;
    }

    void setup() override {
      ESP_LOGCONFIG(TAG, "Setting up SB1 UART...");
      this->rtc_ = global_preferences.make_preference<bool>(this->sensor_->get_object_id_hash());
      this->rtc_.load(&this->safe_mode_);
    }

    void dump_config() override {
      ESP_LOGCONFIG(TAG, "SB1 UART:");
      ESP_LOGCONFIG(TAG, "  Safe Mode: %d", this->safe_mode_);
      ESP_LOGCONFIG(TAG, "  Product Info: %s", this->product_info_);
    }

    /* 
     * State machine; generally follows the same message sequence as
     * has been observed to flow between the stock Tuya firmware and
     * the SB1 chip via the UART bus.
     */
    void loop() override {
      // Register shutdown hooks; don't need to do anything special on shutdown
      // unless we've confirmed the SB1 is awake.
      if (! this->hooks_added_){
        add_late_hooks();
        this->hooks_added_ = true;
      }

      bool have_message = read_message();

      // Reset events are user-initiated and can occur regardless of state
      if (have_message && message_matches(SB1_MESSAGE_TYPE_RESET, 0)) {
          this->safe_mode_ = true;
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
            // Copy product info and add terminating null
            memcpy(this->product_info_, &this->message_.value, this->message_.length);
            memset(this->product_info_ + this->message_.length, 0, 1);
            // Go into config mode and wait for OTA if we've been asked to reset
            if (this->safe_mode_) {
              if (global_wifi_component->has_ap()) {
                set_state(SB1_STATE_CONF_AP);
              } else {
                set_state(SB1_STATE_CONF_STA);
              }
            } else { 
              set_state(SB1_STATE_BOOT_WIFI);
            }
          } else if (state_duration() > ACK_WAIT_TIMEOUT) {
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
            this->safe_mode_ = false;
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
          } else if (state_duration() > OTA_REBOOT_DELAY) {
            safe_reboot("sb1-max-uptime");
          }
          break;
        case SB1_STATE_MOTION_ACK:
          if (state_duration() > MOTION_ACK_DELAY) {
            safe_reboot("sb1-motion");
          }
          break;
        case SB1_STATE_RESET_ACK:
          if (state_duration() > RESET_ACK_DELAY) {
            safe_reboot("sb1-reset");
          }
          break;
      }
    }

};

#endif // SB1_UART_H_
