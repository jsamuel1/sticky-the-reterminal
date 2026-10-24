// tiny_pmu_util — the pure halves of the AMOLED-1.8 power and clock drivers.
//
// AXP2101 PMU (sticky_pmu_axp2101.cpp) and PCF85063A RTC
// (sticky_rtc_pcf85063.cpp) both talk I2C; everything here is the byte
// decoding and encoding between those transactions, with no ESP-IDF include,
// so tests/host/pmu_host_test.sh compiles this exact file on the host.
//
// Register meanings are from the vendor's XPowersLib (examples/esp-idf/
// 90_axp2101_pmu, REG/AXP2101Constants.h + XPowersAXP2101.tpp) and SensorLib
// (REG/PCF85063Constants.h) in waveshareteam/ESP32-S3-Touch-AMOLED-1.8, and
// the vendor's read-only status reader (firmware/brookesia/components/
// system_status). Nothing here writes a power rail.
#pragma once

#include <stdint.h>

// ---- AXP2101 ---------------------------------------------------------------

namespace axp2101 {

constexpr uint8_t kRegStatus1 = 0x00;      // bit5 VBUS good, bit3 battery present
constexpr uint8_t kRegStatus2 = 0x01;      // [7:5] current direction, [2:0] charger
constexpr uint8_t kRegChipId = 0x03;       // reads kChipId on an AXP2101
constexpr uint8_t kChipId = 0x4A;
constexpr uint8_t kRegGaugeCtrl = 0x18;    // bit3 fuel gauge enable
constexpr uint8_t kRegPwroffEn = 0x22;     // power-off sources (logged only)
constexpr uint8_t kRegKeyTiming = 0x27;    // PWR key IRQ/off timing (logged only)
constexpr uint8_t kRegAdcCtrl = 0x30;      // ADC channel enables
constexpr uint8_t kRegAdcBatHi = 0x34;     // 0x34..0x3B: VBAT, TS, VBUS, VSYS
constexpr uint8_t kRegIntEn1 = 0x40;       // 0x40..0x42 IRQ enables
constexpr uint8_t kRegIntSts1 = 0x48;      // 0x48..0x4A IRQ status, write 1 to clear
constexpr uint8_t kRegBatDetCtrl = 0x68;   // bit0 battery detection
constexpr uint8_t kRegDcOnOff = 0x80;      // DCDC1..5 enable bits (read only here)
constexpr uint8_t kRegLdoOnOff0 = 0x90;    // ALDO1..4, BLDO1..2, CPUSLDO, DLDO1
constexpr uint8_t kRegLdoOnOff1 = 0x91;    // DLDO2
constexpr uint8_t kRegBatPercent = 0xA4;   // fuel gauge state of charge, 0..100

// ADC channel enables in kRegAdcCtrl.
constexpr uint8_t kAdcBat = 1U << 0;
constexpr uint8_t kAdcTs = 1U << 1;   // NTC pin: the board has no NTC
constexpr uint8_t kAdcVbus = 1U << 2;
constexpr uint8_t kAdcVsys = 1U << 3;

// IRQ bits of INTEN2 / INTSTS2 (XPowersLib bits 8..15, shifted down by 8).
constexpr uint8_t kIrq2PkeyLong = 1U << 2;
constexpr uint8_t kIrq2PkeyShort = 1U << 3;
constexpr uint8_t kIrq2VbusRemove = 1U << 6;
constexpr uint8_t kIrq2VbusInsert = 1U << 7;

// The ADC channels the battery/supply readers need. Turned on by OR-ing them
// into what is there; every other bit, TS (the NTC channel) included, is
// kept as found. XPowersLib turns TS off on begin(), but the vendor's
// shipped firmware (brookesia system_status) never writes the PMU at all,
// and the charger is the one thing here that must keep doing what it does.
constexpr uint8_t kAdcMeasure = kAdcBat | kAdcVbus | kAdcVsys;

struct Status {
    bool vbus_good = false;    // STATUS1 bit5
    bool bat_present = false;  // STATUS1 bit3
    bool vbus_in = false;      // VBUS good and STATUS2 bit3 clear (XPowers isVbusIn)
    int direction = 3;         // STATUS2[7:5]: 0 standby, 1 charge, 2 discharge
    int charger = 0;           // STATUS2[2:0]: see charger_name()
};

constexpr Status decode_status(uint8_t s1, uint8_t s2)
{
    Status s;
    s.vbus_good = (s1 & (1U << 5)) != 0;
    s.bat_present = (s1 & (1U << 3)) != 0;
    s.vbus_in = s.vbus_good && (s2 & (1U << 3)) == 0;
    s.direction = (s2 >> 5) & 0x07;
    s.charger = s2 & 0x07;
    return s;
}

constexpr bool charging(const Status &s) { return s.direction == 1; }
constexpr bool charge_done(const Status &s) { return s.charger == 4; }

inline const char *charger_name(int state)
{
    switch (state) {
        case 0: return "trickle";
        case 1: return "precharge";
        case 2: return "constant-current";
        case 3: return "constant-voltage";
        case 4: return "done";
        case 5: return "not charging";
        default: return "unknown";
    }
}

// ADC words: VBAT is 13 bits, VBUS and VSYS 14 bits, all 1 mV per LSB.
constexpr int adc13(uint8_t hi, uint8_t lo) { return ((hi & 0x1F) << 8) | lo; }
constexpr int adc14(uint8_t hi, uint8_t lo) { return ((hi & 0x3F) << 8) | lo; }

// The 8-byte block read from kRegAdcBatHi.
struct Adc {
    int vbat_mv = 0;
    int vbus_mv = 0;
    int vsys_mv = 0;
};

constexpr Adc decode_adc(const uint8_t b[8])
{
    Adc a;
    a.vbat_mv = adc13(b[0], b[1]);
    a.vbus_mv = adc14(b[4], b[5]);
    a.vsys_mv = adc14(b[6], b[7]);
    return a;
}

enum class KeyEvent : uint8_t { None, Short, Long };

// One PWR key event from INTSTS2. A long press also latches nothing else on
// the AXP2101, but if both bits are ever set at once (two presses inside one
// poll) the long one wins: it is the stronger intent.
constexpr KeyEvent key_event(uint8_t intsts2)
{
    return (intsts2 & kIrq2PkeyLong)    ? KeyEvent::Long
           : (intsts2 & kIrq2PkeyShort) ? KeyEvent::Short
                                        : KeyEvent::None;
}

// ---- init sequence ----
//
// The writes init() makes after the chip ID matches, in order, each one
// required: without the ADC channels there is no voltage, without battery
// detection STATUS1 bit3 means nothing, without the gauge the percent
// register is not a reading, without the key IRQs the PWR key is dead, and
// a stale key bit left latched would fire a press nobody made. So the first
// failure ends the sequence and is returned; the driver then drops its
// device handle, every reader answers ESP_ERR_INVALID_STATE, and a later
// init() runs the whole sequence again.
//
// Bus is anything with
//   int rd(uint8_t reg, uint8_t &value);    0 = ok, else the error
//   int wr(uint8_t reg, uint8_t value);
//   void changed(const char *what, uint8_t reg, uint8_t before,
//                uint8_t after, int err);   one call per write made
// so the host test drives this exact sequence against a fake register file.

struct SetupStep {
    uint8_t reg;
    uint8_t bits;      // set_bits: OR these in, keep the rest
    bool clear_write;  // write `bits` as-is (write-1-to-clear), no read
    const char *what;
};

constexpr SetupStep kSetup[] = {
    {kRegAdcCtrl, kAdcMeasure, false, "ADC VBAT/VBUS/VSYS"},
    {kRegBatDetCtrl, 1U << 0, false, "battery detect"},
    {kRegGaugeCtrl, 1U << 3, false, "fuel gauge"},
    {kRegIntEn1 + 1, kIrq2PkeyShort | kIrq2PkeyLong, false, "PWR key IRQ"},
    // Drop a press latched before boot (the one that powered us on, or a
    // stale one): the key reader must only see presses made from here on.
    {kRegIntSts1 + 1, kIrq2PkeyShort | kIrq2PkeyLong, true, "stale PWR key clear"},
};

struct SetupResult {
    int err = 0;                  // 0 = every step done
    const char *step = nullptr;   // the step that failed
    bool read_failed = false;     // the failure was the read, not the write
};

template <typename Bus>
SetupResult run_setup(Bus &bus)
{
    SetupResult r;
    for (const SetupStep &s : kSetup) {
        if (s.clear_write) {
            const int e = bus.wr(s.reg, s.bits);
            bus.changed(s.what, s.reg, 0, s.bits, e);
            if (e) { r.err = e; r.step = s.what; return r; }
            continue;
        }
        uint8_t v = 0;
        int e = bus.rd(s.reg, v);
        if (e) { r.err = e; r.step = s.what; r.read_failed = true; return r; }
        const uint8_t nv = static_cast<uint8_t>(v | s.bits);
        if (nv == v) continue;  // already as needed: no write
        e = bus.wr(s.reg, nv);
        bus.changed(s.what, s.reg, v, nv, e);
        if (e) { r.err = e; r.step = s.what; return r; }
    }
    return r;
}

}  // namespace axp2101

// ---- PCF85063A -------------------------------------------------------------

namespace pcf85063 {

constexpr uint8_t kRegCtrl1 = 0x00;   // bit5 STOP, bit1 12-hour mode
constexpr uint8_t kRegSeconds = 0x04; // 0x04..0x0A: sec(bit7 OS), min, hour,
                                      // day, weekday, month, year (2000-based)
constexpr uint8_t kCtrl1Stop = 1U << 5;
constexpr uint8_t kCtrl1Mode12h = 1U << 1;
constexpr uint8_t kOscStop = 0x80;

struct Fields {
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
};

enum class Decode : uint8_t { Ok, OscillatorStopped, BadBcd, OutOfRange };

constexpr bool valid_bcd(uint8_t v) { return (v & 0x0F) <= 9 && (v >> 4) <= 9; }
constexpr int from_bcd(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
constexpr uint8_t to_bcd(int v) { return static_cast<uint8_t>(((v / 10) << 4) | (v % 10)); }

constexpr bool leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

constexpr int days_in(int y, int m)
{
    return m == 2 ? (leap(y) ? 29 : 28)
                  : (m == 4 || m == 6 || m == 9 || m == 11) ? 30 : 31;
}

// The chip's year register is 00..99 with no century bit: 2000..2099.
constexpr bool valid(const Fields &f)
{
    return f.year >= 2000 && f.year <= 2099 && f.month >= 1 && f.month <= 12 &&
           f.day >= 1 && f.day <= days_in(f.year, f.month) && f.hour >= 0 &&
           f.hour <= 23 && f.minute >= 0 && f.minute <= 59 && f.second >= 0 &&
           f.second <= 59;
}

// 0 = Sunday, the PCF85063A weekday encoding (Zeller's congruence).
constexpr int weekday(int y, int m, int d)
{
    const int mm = m < 3 ? m + 12 : m;
    const int yy = m < 3 ? y - 1 : y;
    return (d + (13 * (mm + 1)) / 5 + yy + yy / 4 - yy / 100 + yy / 400 + 6) % 7;
}

// raw = the 7 bytes read from kRegSeconds (24-hour mode).
inline Decode decode(const uint8_t raw[7], Fields &out)
{
    if (raw[0] & kOscStop) return Decode::OscillatorStopped;
    const uint8_t sec = raw[0] & 0x7F, min = raw[1] & 0x7F, hour = raw[2] & 0x3F,
                  day = raw[3] & 0x3F, month = raw[5] & 0x1F, year = raw[6];
    if (!valid_bcd(sec) || !valid_bcd(min) || !valid_bcd(hour) || !valid_bcd(day) ||
        !valid_bcd(month) || !valid_bcd(year))
        return Decode::BadBcd;
    Fields f;
    f.second = from_bcd(sec);
    f.minute = from_bcd(min);
    f.hour = from_bcd(hour);
    f.day = from_bcd(day);
    f.month = from_bcd(month);
    f.year = 2000 + from_bcd(year);
    if (!valid(f)) return Decode::OutOfRange;
    out = f;
    return Decode::Ok;
}

// out = the 7 bytes to write at kRegSeconds. Seconds bit7 (OS) is written 0,
// which declares the time good. Caller checks valid() first.
inline void encode(const Fields &f, uint8_t out[7])
{
    out[0] = to_bcd(f.second);
    out[1] = to_bcd(f.minute);
    out[2] = to_bcd(f.hour);
    out[3] = to_bcd(f.day);
    out[4] = static_cast<uint8_t>(weekday(f.year, f.month, f.day));
    out[5] = to_bcd(f.month);
    out[6] = to_bcd(f.year - 2000);
}

}  // namespace pcf85063
