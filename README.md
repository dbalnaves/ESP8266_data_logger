# High resolution data logger for the ESP8266

This data logger is to be a platform for taking multiple high resolution readings and uploading to InfluxDB for presentation in Grafana.  This uses the Espressif SDK as opposed to the open SDK as I wanted to leaverage their autoconfig app.  I ported the DHT code from the esp-open-sdk; took a little rework to get working.

As the ESP8266 RTC is rubbish, this uses NTP to calculate the offsets from the system clock.  To do this, we expose the get_boot_time() in the Espressif SDK:

components/newlib/newlib/port/time.c
```
-static inline uint64_t get_boot_time()
+uint64_t get_boot_time()

```

## Bugs

While elegant, the ESP HTTP client library doesn't appear thread safe.  This becomes a problem if your not rebooting every couple of hours.
