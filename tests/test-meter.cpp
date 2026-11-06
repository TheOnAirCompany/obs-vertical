#include "test.hpp"

#include "meter.hpp"

using namespace vertical::core;

TEST("meter: dB conversion")
{
	CHECK_NEAR(mulToDb(1.0f), 0.0, 1e-4);
	CHECK_NEAR(mulToDb(0.5f), -6.0206, 1e-3);
	CHECK_NEAR(mulToDb(0.1f), -20.0, 1e-3);
	CHECK_NEAR(mulToDb(0.001f), -60.0, 1e-3);
	CHECK_EQ(mulToDb(0.0f), kMeterSilenceDb);
	CHECK_EQ(mulToDb(-1.0f), kMeterSilenceDb);
}

TEST("meter: peak of samples")
{
	const float s[] = {0.1f, -0.7f, 0.3f, 0.65f};
	CHECK_NEAR(peakOf(s, 4), 0.7, 1e-6);
	CHECK_NEAR(peakOf(s, 1), 0.1, 1e-6);
	CHECK_EQ(peakOf(nullptr, 4), 0.0f);
	CHECK_EQ(peakOf(s, 0), 0.0f);
}

TEST("meter: scale and zones like the OBS mixer")
{
	CHECK_EQ(meterFraction(-70.0f), 0.0f);
	CHECK_EQ(meterFraction(kMeterSilenceDb), 0.0f);
	CHECK_NEAR(meterFraction(-30.0f), 0.5, 1e-6);
	CHECK_NEAR(meterFraction(-20.0f), 2.0 / 3.0, 1e-6);
	CHECK_EQ(meterFraction(0.0f), 1.0f);
	CHECK_EQ(meterFraction(3.0f), 1.0f);
	CHECK(meterZone(-40.0f) == MeterZone::Nominal);
	CHECK(meterZone(-20.0f) == MeterZone::Warning);
	CHECK(meterZone(-10.0f) == MeterZone::Warning);
	CHECK(meterZone(-9.0f) == MeterZone::Error);
	CHECK(meterZone(0.0f) == MeterZone::Error);
}

TEST("meter: ballistics jump up, decay at 11.76 dB/s, then silence")
{
	MeterBallistics b;
	CHECK_EQ(b.level(), kMeterSilenceDb);
	CHECK_NEAR(b.update(-12.0f, 0.016), -12.0, 1e-6);
	/* Quieter audio: the bar falls at the decay rate, not at once. */
	CHECK_NEAR(b.update(-40.0f, 1.0), -12.0 - kMeterDecayDbPerSec, 1e-4);
	/* Louder: jumps. */
	CHECK_NEAR(b.update(-3.0f, 0.016), -3.0, 1e-6);
	/* Silence: falls, then stops at "no signal" once below the scale. */
	for (int i = 0; i < 400; i++)
		b.update(kMeterSilenceDb, 0.016);
	CHECK_EQ(b.level(), kMeterSilenceDb);
	CHECK_EQ(meterFraction(b.level()), 0.0f);
	b.update(-6.0f, 0.016);
	b.reset();
	CHECK_EQ(b.level(), kMeterSilenceDb);
}
