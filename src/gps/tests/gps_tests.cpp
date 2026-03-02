#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "gps/NmeaParser.h"

using Catch::Approx;

// ---- checksum ---------------------------------------------------------------

TEST_CASE("computeChecksum extracts XOR between $ and *", "[nmea]")
{
    // Body: GPGGA,123519,...  XOR = 0x47
    CHECK(nmea::computeChecksum(
        "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47") == 0x47);

    // RMC body XOR = 0x6A
    CHECK(nmea::computeChecksum(
        "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A") == 0x6A);

    // No $ or * → returns 0
    CHECK(nmea::computeChecksum("GPGGA,no delimiters") == 0);
}

// ---- parseGGA ---------------------------------------------------------------

TEST_CASE("parseGGA — valid NMEA spec example", "[nmea][gga]")
{
    // NMEA specification example sentence (checksum verified: 0x47)
    // Lat: 4807.038 N  = 48 + 7.038/60 = 48.1173°
    // Lon: 01131.000 E = 11 + 31.000/60 ≈ 11.5167°
    // Alt: 545.4 m
    const std::string s =
        "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47";

    nmea::Fix fix{};
    REQUIRE(nmea::parseGGA(s, fix) == true);
    CHECK(fix.valid == true);
    CHECK(fix.lat_deg  == Approx(48.1173).epsilon(0.0001));
    CHECK(fix.lon_deg  == Approx(11.5167).epsilon(0.001));
    CHECK(fix.alt_msl_m == Approx(545.4f).epsilon(0.1f));
    // GGA has no course/speed — should be zero
    CHECK(fix.course_deg == Approx(0.f).margin(0.001f));
    CHECK(fix.speed_mps  == Approx(0.f).margin(0.001f));
}

TEST_CASE("parseGGA — GNGGA tag accepted", "[nmea][gga]")
{
    // Same data but with GNGGA talker ID (GNSS combined).
    // Recompute checksum: swap GPGGA→GNGGA changes G→N at position 2.
    // 0x47 XOR 'P'=0x50 XOR 'N'=0x4E = 0x47^0x50^0x4E = 0x17^0x4E = 0x59
    const std::string s =
        "$GNGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*59";

    nmea::Fix fix{};
    REQUIRE(nmea::parseGGA(s, fix) == true);
    CHECK(fix.valid == true);
    CHECK(fix.lat_deg == Approx(48.1173).epsilon(0.0001));
}

TEST_CASE("parseGGA — fix quality 0 rejected", "[nmea][gga]")
{
    // Change quality field from 1 → 0. New checksum = 0x47 XOR '1'=0x31 XOR '0'=0x30 = 0x46
    const std::string s =
        "$GPGGA,123519,4807.038,N,01131.000,E,0,08,0.9,545.4,M,46.9,M,,*46";

    nmea::Fix fix{};
    REQUIRE(nmea::parseGGA(s, fix) == false);
    CHECK(fix.valid == false);
}

TEST_CASE("parseGGA — bad checksum rejected", "[nmea][gga]")
{
    // Corrupt last byte of checksum
    const std::string s =
        "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*48";

    nmea::Fix fix{};
    REQUIRE(nmea::parseGGA(s, fix) == false);
}

TEST_CASE("parseGGA — rejects non-GGA sentence", "[nmea][gga]")
{
    nmea::Fix fix{};
    REQUIRE(nmea::parseGGA(
        "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A",
        fix) == false);
}

TEST_CASE("parseGGA — Southern hemisphere latitude is negative", "[nmea][gga]")
{
    // Fallback sentence: 3349.7500 S = -(33 + 49.75/60) = -33.8292°
    // Checksum 0x67 (pre-verified)
    const std::string s =
        "$GPGGA,000000.00,3349.7500,S,15105.5000,E,1,08,0.9,42.3,M,-33.5,M,,*67";

    nmea::Fix fix{};
    REQUIRE(nmea::parseGGA(s, fix) == true);
    CHECK(fix.lat_deg < 0.0);
    CHECK(fix.lat_deg == Approx(-33.8292).epsilon(0.001));
    CHECK(fix.lon_deg == Approx(151.0917).epsilon(0.001));
}

// ---- parseRMC ---------------------------------------------------------------

TEST_CASE("parseRMC — valid NMEA spec example", "[nmea][rmc]")
{
    // NMEA specification example sentence (checksum verified: 0x6A)
    // Lat: 48.1173°  Lon: 11.5167°
    // Speed: 22.4 knots = 22.4 * 0.514444 ≈ 11.524 m/s
    // Course: 84.4°
    const std::string s =
        "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A";

    nmea::Fix fix{};
    REQUIRE(nmea::parseRMC(s, fix) == true);
    CHECK(fix.valid == true);
    CHECK(fix.lat_deg    == Approx(48.1173).epsilon(0.0001));
    CHECK(fix.lon_deg    == Approx(11.5167).epsilon(0.001));
    CHECK(fix.speed_mps  == Approx(11.524f).epsilon(0.01f));
    CHECK(fix.course_deg == Approx(84.4f).epsilon(0.01f));
    // RMC has no altitude from this parser — left zero
    CHECK(fix.alt_msl_m  == Approx(0.f).margin(0.001f));
}

TEST_CASE("parseRMC — GNRMC tag accepted", "[nmea][rmc]")
{
    // GPRMC→GNRMC: P=0x50→N=0x4E, checksum change = 0x6A^0x50^0x4E = 0x6A^0x1E = 0x74
    const std::string s =
        "$GNRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*74";

    nmea::Fix fix{};
    REQUIRE(nmea::parseRMC(s, fix) == true);
    CHECK(fix.valid == true);
}

TEST_CASE("parseRMC — status V (void) rejected", "[nmea][rmc]")
{
    // Change A → V at status field. Checksum: 0x6A^'A'^'V' = 0x6A^0x41^0x56 = 0x7D
    const std::string s =
        "$GPRMC,123519,V,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*7D";

    nmea::Fix fix{};
    REQUIRE(nmea::parseRMC(s, fix) == false);
    CHECK(fix.valid == false);
}

TEST_CASE("parseRMC — bad checksum rejected", "[nmea][rmc]")
{
    const std::string s =
        "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6B";

    nmea::Fix fix{};
    REQUIRE(nmea::parseRMC(s, fix) == false);
}

TEST_CASE("parseRMC — rejects non-RMC sentence", "[nmea][rmc]")
{
    nmea::Fix fix{};
    REQUIRE(nmea::parseRMC(
        "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47",
        fix) == false);
}

// ---- parse (dispatch) -------------------------------------------------------

TEST_CASE("parse — dispatches GGA and RMC correctly", "[nmea]")
{
    nmea::Fix fix{};

    REQUIRE(nmea::parse(
        "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47",
        fix) == true);
    CHECK(fix.valid == true);

    REQUIRE(nmea::parse(
        "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A",
        fix) == true);
    CHECK(fix.valid == true);
}

TEST_CASE("parse — unrecognised sentence returns false", "[nmea]")
{
    nmea::Fix fix{};
    REQUIRE(nmea::parse("$GPGSV,3,1,11,...*XX", fix) == false);
    REQUIRE(nmea::parse("", fix) == false);
    REQUIRE(nmea::parse("not nmea at all", fix) == false);
}

// ---- DDmm.mmmm conversion (tested implicitly; verify edge cases) ------------

TEST_CASE("parseGGA — zero speed/course left at zero from GGA-only data", "[nmea]")
{
    nmea::Fix fix{};
    nmea::parseGGA(
        "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47",
        fix);
    // GGA does not supply course or speed; parser must leave them zero
    CHECK(fix.course_deg == Approx(0.f).margin(1e-4f));
    CHECK(fix.speed_mps  == Approx(0.f).margin(1e-4f));
}
