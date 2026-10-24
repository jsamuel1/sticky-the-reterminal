// pmu host test: compiles the SHIPPED firmware/main/tiny/tiny_pmu_util.h and
// pins the AXP2101 status/ADC/key decoding and the PCF85063A BCD round trip
// against register values worked out from the vendor's XPowersLib/SensorLib.
#include "tiny/tiny_pmu_util.h"

#include <cstdio>

static int fails = 0;
#define CHECK(name, cond) do { \
    if (cond) printf("ok   %s\n", name); \
    else { printf("FAIL %s\n", name); fails++; } \
} while (0)

static void axp_status() {
    // STATUS1 0x28: VBUS good (bit5) + battery present (bit3).
    // STATUS2 0x22: direction 1 (charging), charger 2 (constant current).
    auto s = axp2101::decode_status(0x28, 0x22);
    CHECK("status: vbus good", s.vbus_good);
    CHECK("status: battery present", s.bat_present);
    CHECK("status: vbus in", s.vbus_in);
    CHECK("status: charging", axp2101::charging(s));
    CHECK("status: charger CC", s.charger == 2);
    CHECK("status: not done", !axp2101::charge_done(s));

    // Discharging on battery, no USB.
    s = axp2101::decode_status(0x08, 0x45);
    CHECK("status: no vbus", !s.vbus_good && !s.vbus_in);
    CHECK("status: discharging is not charging", !axp2101::charging(s) && s.direction == 2);
    CHECK("status: charger not charging", s.charger == 5);

    // USB in, cell full: standby, charger done.
    s = axp2101::decode_status(0x28, 0x04);
    CHECK("status: standby + done", s.direction == 0 && axp2101::charge_done(s));
    CHECK("status: full is not charging", !axp2101::charging(s));

    // VBUS good but STATUS2 bit3 set: XPowers isVbusIn() says not in.
    s = axp2101::decode_status(0x20, 0x08);
    CHECK("status: vbus good, bit3 set -> not in", s.vbus_good && !s.vbus_in);

    // USB only, no cell.
    s = axp2101::decode_status(0x20, 0x00);
    CHECK("status: no battery", !s.bat_present);
    CHECK("charger name done", axp2101::charger_name(4)[0] == 'd');
    CHECK("charger name out of range", axp2101::charger_name(7)[0] == 'u');
}

static void axp_adc() {
    // VBAT 3987 mV = 0x0F93 (13-bit), stray high bits must be masked.
    // VBUS 5021 mV = 0x139D, VSYS 4986 mV = 0x137A (14-bit).
    const uint8_t b[8] = {0xEF, 0x93, 0x00, 0x00, 0xD3, 0x9D, 0x13, 0x7A};
    const auto a = axp2101::decode_adc(b);
    CHECK("adc: vbat 13-bit masked", a.vbat_mv == 3987);
    CHECK("adc: vbus 14-bit masked", a.vbus_mv == 5021);
    CHECK("adc: vsys", a.vsys_mv == 4986);
    CHECK("adc mask: only measure channels",
          axp2101::kAdcMeasure == 0x0D && !(axp2101::kAdcMeasure & axp2101::kAdcTs));
}

static void axp_key() {
    using axp2101::KeyEvent;
    CHECK("key: none", axp2101::key_event(0x00) == KeyEvent::None);
    CHECK("key: vbus bits are not a key", axp2101::key_event(0xC0) == KeyEvent::None);
    CHECK("key: short (bit3)", axp2101::key_event(0x08) == KeyEvent::Short);
    CHECK("key: long (bit2)", axp2101::key_event(0x04) == KeyEvent::Long);
    CHECK("key: both -> long", axp2101::key_event(0x0C) == KeyEvent::Long);
    // XPowersLib PKEY_SHORT_IRQ = _BV(11), PKEY_LONG_IRQ = _BV(10): INTSTS2.
    CHECK("key bits match XPowersLib", axp2101::kIrq2PkeyShort == (1u << (11 - 8)) &&
                                           axp2101::kIrq2PkeyLong == (1u << (10 - 8)));
}

static void rtc_cases() {
    // 2026-10-24 21:07:45, Saturday (6).
    pcf85063::Fields f;
    f.year = 2026; f.month = 10; f.day = 24; f.hour = 21; f.minute = 7; f.second = 45;
    uint8_t raw[7] = {};
    pcf85063::encode(f, raw);
    CHECK("encode: seconds BCD, OS clear", raw[0] == 0x45);
    CHECK("encode: hour BCD", raw[2] == 0x21);
    CHECK("encode: weekday saturday", raw[4] == 6);
    CHECK("encode: month BCD", raw[5] == 0x10);
    CHECK("encode: year 2000-based", raw[6] == 0x26);
    pcf85063::Fields g;
    CHECK("decode: round trip ok", pcf85063::decode(raw, g) == pcf85063::Decode::Ok);
    CHECK("decode: round trip value", g.year == 2026 && g.month == 10 && g.day == 24 &&
                                          g.hour == 21 && g.minute == 7 && g.second == 45);

    uint8_t stopped[7] = {0xC5, 0x07, 0x21, 0x24, 6, 0x10, 0x26};
    CHECK("decode: oscillator stop",
          pcf85063::decode(stopped, g) == pcf85063::Decode::OscillatorStopped);
    uint8_t bad[7] = {0x4A, 0x07, 0x21, 0x24, 6, 0x10, 0x26};
    CHECK("decode: bad BCD", pcf85063::decode(bad, g) == pcf85063::Decode::BadBcd);
    uint8_t feb30[7] = {0x00, 0x00, 0x00, 0x30, 1, 0x02, 0x26};
    CHECK("decode: feb 30 out of range",
          pcf85063::decode(feb30, g) == pcf85063::Decode::OutOfRange);
    uint8_t leap[7] = {0x00, 0x00, 0x00, 0x29, 4, 0x02, 0x28};
    CHECK("decode: 2028-02-29 ok", pcf85063::decode(leap, g) == pcf85063::Decode::Ok);
    // Unused high bits (minute bit7, weekday) do not leak into values.
    uint8_t junk[7] = {0x10, 0x80 | 0x07, 0xC0 | 0x09, 0xC0 | 0x01, 0xF8 | 2, 0xE0 | 0x01, 0x30};
    CHECK("decode: masks unused bits", pcf85063::decode(junk, g) == pcf85063::Decode::Ok &&
                                           g.minute == 7 && g.hour == 9 && g.day == 1 &&
                                           g.month == 1 && g.year == 2030);

    pcf85063::Fields old;
    old.year = 1999; old.month = 12; old.day = 31;
    CHECK("valid: 1999 refused (no century bit)", !pcf85063::valid(old));
    CHECK("weekday: 2000-01-01 saturday", pcf85063::weekday(2000, 1, 1) == 6);
    CHECK("weekday: 2024-02-29 thursday", pcf85063::weekday(2024, 2, 29) == 4);
}

int main() {
    axp_status();
    axp_adc();
    axp_key();
    rtc_cases();
    if (fails) { printf("%d FAILED\n", fails); return 1; }
    return 0;
}
