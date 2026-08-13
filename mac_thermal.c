//clang -fmodules -framework IOKit -framework CoreFoundation mac_thermal.c -o mac_thermal
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hidsystem/IOHIDEventSystemClient.h>
#include <IOKit/hidsystem/IOHIDServiceClient.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct CF_BRIDGED_TYPE(id) __IOHIDEvent *IOHIDEventRef;

extern IOHIDEventSystemClientRef IOHIDEventSystemClientCreate(CFAllocatorRef allocator);
extern int IOHIDEventSystemClientSetMatching(IOHIDEventSystemClientRef client, CFDictionaryRef matching);
extern IOHIDEventRef IOHIDServiceClientCopyEvent(IOHIDServiceClientRef service, int64_t type, int32_t options, int64_t timeout);
extern double IOHIDEventGetFloatValue(IOHIDEventRef event, int64_t field);

enum {
    kTempEventType = 15,
    kTempEventField = 15LL << 16,
    kAppleVendorPage = 0xff00,
    kAppleVendorTemperatureUsage = 0x0005,
};

typedef struct {
    char name[160];
    double temp_c;
    int valid;
} SensorReading;

static CFDictionaryRef CreateTemperatureMatching(void) {
    int page = kAppleVendorPage;
    int usage = kAppleVendorTemperatureUsage;
    CFNumberRef pageNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &page);
    CFNumberRef usageNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &usage);

    const void *keys[] = { CFSTR("PrimaryUsagePage"), CFSTR("PrimaryUsage") };
    const void *values[] = { pageNum, usageNum };
    CFDictionaryRef dict = CFDictionaryCreate(kCFAllocatorDefault,
                                              keys,
                                              values,
                                              2,
                                              &kCFTypeDictionaryKeyCallBacks,
                                              &kCFTypeDictionaryValueCallBacks);
    CFRelease(pageNum);
    CFRelease(usageNum);
    return dict;
}

static int CopyCFString(CFStringRef string, char *buf, size_t bufSize) {
    if (!string) {
        return 0;
    }
    return CFStringGetCString(string, buf, (CFIndex)bufSize, kCFStringEncodingUTF8) != 0;
}

static int CompareSensors(const void *a, const void *b) {
    const SensorReading *left = (const SensorReading *)a;
    const SensorReading *right = (const SensorReading *)b;
    return strcmp(left->name, right->name);
}

static int ReadSensors(SensorReading *sensors, int maxSensors) {
    CFDictionaryRef matching = CreateTemperatureMatching();
    IOHIDEventSystemClientRef client = IOHIDEventSystemClientCreate(kCFAllocatorDefault);
    if (!client) {
        CFRelease(matching);
        return -1;
    }

    IOHIDEventSystemClientSetMatching(client, matching);
    CFArrayRef services = IOHIDEventSystemClientCopyServices(client);
    if (!services) {
        CFRelease(client);
        CFRelease(matching);
        return -1;
    }

    int count = 0;
    CFIndex serviceCount = CFArrayGetCount(services);
    for (CFIndex i = 0; i < serviceCount && count < maxSensors; i++) {
        IOHIDServiceClientRef service = (IOHIDServiceClientRef)CFArrayGetValueAtIndex(services, i);
        CFStringRef product = IOHIDServiceClientCopyProperty(service, CFSTR("Product"));
        IOHIDEventRef event = IOHIDServiceClientCopyEvent(service, kTempEventType, 0, 0);
        if (!event) {
            if (product) CFRelease(product);
            continue;
        }

        double temp = IOHIDEventGetFloatValue(event, kTempEventField);
        CFRelease(event);

        if (temp > 0.0 && temp < 150.0) {
            SensorReading *reading = &sensors[count];
            memset(reading, 0, sizeof(*reading));
            if (product && CopyCFString(product, reading->name, sizeof(reading->name))) {
                reading->valid = 1;
            } else {
                snprintf(reading->name, sizeof(reading->name), "sensor-%ld", (long)i);
                reading->valid = 1;
            }
            reading->temp_c = temp;
            count++;
        }

        if (product) CFRelease(product);
    }

    CFRelease(services);
    CFRelease(client);
    CFRelease(matching);

    qsort(sensors, (size_t)count, sizeof(SensorReading), CompareSensors);
    return count;
}

static int IsDieSensor(const char *name) {
    return strstr(name, "tdie") != NULL;
}

static const char *SensorKind(const char *name) {
    if (strncmp(name, "PMU tdie", 8) == 0) {
        return "cpu-die";
    }
    if (strncmp(name, "PMU2 tdie", 9) == 0) {
        return "gpu-die";
    }
    return "other";
}

static int FindHotspot(const SensorReading *sensors, int count, SensorReading *out) {
    int found = 0;
    for (int i = 0; i < count; i++) {
        if (!sensors[i].valid || !IsDieSensor(sensors[i].name)) {
            continue;
        }
        if (!found || sensors[i].temp_c > out->temp_c) {
            *out = sensors[i];
            found = 1;
        }
    }
    return found;
}

static void PrintReadingHuman(const SensorReading *reading) {
    printf("%.2f C  %s\n", reading->temp_c, reading->name);
}

static void PrintHotspotHuman(const SensorReading *reading) {
    printf("SoC hotspot: %.2f C  %s\n", reading->temp_c, reading->name);
}

static void PrintListHuman(const SensorReading *sensors, int count) {
    for (int i = 0; i < count; i++) {
        const char *kind = SensorKind(sensors[i].name);
        printf("%-8s %6.2f C  %s\n", kind, sensors[i].temp_c, sensors[i].name);
    }
}

static void PrintListJSON(const SensorReading *sensors, int count) {
    printf("[");
    for (int i = 0; i < count; i++) {
        if (i > 0) {
            printf(",");
        }
        printf("{\"name\":\"%s\",\"temp_c\":%.2f,\"kind\":\"%s\"}",
               sensors[i].name,
               sensors[i].temp_c,
               SensorKind(sensors[i].name));
    }
    printf("]\n");
}

static void PrintHotspotJSON(const SensorReading *reading) {
    printf("{\"sensor\":\"%s\",\"temp_c\":%.2f}\n", reading->name, reading->temp_c);
}

static void PrintUsage(const char *argv0) {
    fprintf(stderr,
            "Usage: %s [options]\n"
            "\n"
            "Read Apple Silicon SoC temperature sensors via IOHID events.\n"
            "The default metric is the hottest die sensor (name contains \"tdie\").\n"
            "\n"
            "Options:\n"
            "  -l, --list              list all temperature sensors\n"
            "  -s, --sensor NAME       print one sensor (exact or unique substring)\n"
            "  -w, --watch [SECONDS]   repeat continuously, default interval 1s\n"
            "  -i, --interval SECONDS  interval for --watch\n"
            "  -j, --json              JSON output\n"
            "  -h, --help              show this help\n",
            argv0);
}

static const SensorReading *FindSensorByName(const SensorReading *sensors,
                                             int count,
                                             const char *name) {
    const SensorReading *exact = NULL;
    const SensorReading *substring = NULL;
    int substringMatches = 0;

    for (int i = 0; i < count; i++) {
        if (strcmp(sensors[i].name, name) == 0) {
            exact = &sensors[i];
        }
        if (strstr(sensors[i].name, name) != NULL) {
            substring = &sensors[i];
            substringMatches++;
        }
    }

    if (exact) {
        return exact;
    }
    if (substringMatches == 1) {
        return substring;
    }
    return NULL;
}

static void RunOnce(const char *sensorName, int jsonOutput, int listOutput) {
    SensorReading sensors[256];
    int count = ReadSensors(sensors, (int)(sizeof(sensors) / sizeof(sensors[0])));
    if (count < 0) {
        fprintf(stderr, "failed to read temperature sensors\n");
        exit(1);
    }
    if (count == 0) {
        fprintf(stderr, "no temperature sensors found\n");
        exit(1);
    }

    if (listOutput) {
        if (jsonOutput) {
            PrintListJSON(sensors, count);
        } else {
            PrintListHuman(sensors, count);
        }
        return;
    }

    if (sensorName) {
        const SensorReading *reading = FindSensorByName(sensors, count, sensorName);
        if (!reading) {
            fprintf(stderr, "sensor not found or ambiguous: %s\n", sensorName);
            exit(1);
        }
        if (jsonOutput) {
            PrintHotspotJSON(reading);
        } else {
            PrintReadingHuman(reading);
        }
        return;
    }

    SensorReading hotspot;
    if (!FindHotspot(sensors, count, &hotspot)) {
        fprintf(stderr, "no die temperature sensors found\n");
        exit(1);
    }
    if (jsonOutput) {
        PrintHotspotJSON(&hotspot);
    } else {
        PrintHotspotHuman(&hotspot);
    }
}

int main(int argc, char **argv) {
    const char *sensorName = NULL;
    int listOutput = 0;
    int jsonOutput = 0;
    int watchOutput = 0;
    double interval = 1.0;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "-l") == 0 || strcmp(arg, "--list") == 0) {
            listOutput = 1;
        } else if (strcmp(arg, "-s") == 0 || strcmp(arg, "--sensor") == 0) {
            if (i + 1 >= argc) {
                PrintUsage(argv[0]);
                return 1;
            }
            sensorName = argv[++i];
        } else if (strcmp(arg, "-w") == 0 || strcmp(arg, "--watch") == 0) {
            watchOutput = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                interval = atof(argv[++i]);
            }
        } else if (strcmp(arg, "-i") == 0 || strcmp(arg, "--interval") == 0) {
            if (i + 1 >= argc) {
                PrintUsage(argv[0]);
                return 1;
            }
            interval = atof(argv[++i]);
        } else if (strcmp(arg, "-j") == 0 || strcmp(arg, "--json") == 0) {
            jsonOutput = 1;
        } else if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            PrintUsage(argv[0]);
            return 0;
        } else {
            PrintUsage(argv[0]);
            return 1;
        }
    }

    if (interval <= 0.0) {
        fprintf(stderr, "interval must be greater than zero\n");
        return 1;
    }

    if (!watchOutput) {
        RunOnce(sensorName, jsonOutput, listOutput);
        return 0;
    }

    useconds_t sleepMicros = (useconds_t)(interval * 1000000.0);
    for (;;) {
        SensorReading sensors[256];
        int count = ReadSensors(sensors, (int)(sizeof(sensors) / sizeof(sensors[0])));
        if (count < 0) {
            fprintf(stderr, "failed to read temperature sensors\n");
            return 1;
        }

        if (listOutput) {
            if (jsonOutput) {
                PrintListJSON(sensors, count);
            } else {
                PrintListHuman(sensors, count);
            }
        } else if (sensorName) {
            const SensorReading *reading = FindSensorByName(sensors, count, sensorName);
            if (!reading) {
                fprintf(stderr, "sensor not found or ambiguous: %s\n", sensorName);
                return 1;
            }
            if (jsonOutput) {
                PrintHotspotJSON(reading);
            } else {
                PrintReadingHuman(reading);
            }
        } else {
            SensorReading hotspot;
            if (!FindHotspot(sensors, count, &hotspot)) {
                fprintf(stderr, "no die temperature sensors found\n");
                return 1;
            }
            if (jsonOutput) {
                PrintHotspotJSON(&hotspot);
            } else {
                PrintHotspotHuman(&hotspot);
            }
        }

        fflush(stdout);
        usleep(sleepMicros);
    }
}
