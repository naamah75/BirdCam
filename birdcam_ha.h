#pragma once

#include <Arduino.h>
#include <time.h>
#include <PubSubClient.h>

// Init (da chiamare una volta dopo che hai calcolato dev_id/topic)
void ha_init(PubSubClient& client,
             const char* dev_id,
             const char* base_topic,
             const char* status_topic,
             const char* fw_version);

// Discovery (da chiamare ad ogni connessione MQTT riuscita)
void ha_publish_discovery();

// Setters (aggiorna cache interna per discovery/telemetria)
void ha_set_boot_time(time_t boot_time_epoch);
void ha_set_wifi(int rssi, int channel);
void ha_set_pmu(uint16_t vbus_mv, uint16_t sys_mv, uint16_t batt_mv,
                bool vbus_present, bool batt_present);

// Telemetria periodica (consigliato 60s)
void ha_publish_periodic(uint32_t now_ms,
                         uint32_t pir_count,
                         int archive_count,
                         const char* ip);

// Eventi PIR
void ha_on_pir(uint32_t pir_count, int archive_count, const char* ip, long ts_epoch);
void ha_pir_off();

// Topic helper per camera (così BirdCam.ino sa dove pubblicare i frame)
const char* ha_topic_cam_snapshot(); // retained frame
const char* ha_topic_cam_stream();   // non-retained frames
