#ifndef INCLUDED_WATTGAUGE_H
#define INCLUDED_WATTGAUGE_H

/**
 * WattGauge attempts to approximate current Watt (Joule/s)
 * production/consumption based on a regular input of current absolute
 * (increasing) watt hour values.
 *
 * For power/electricity meters that do not provide "current" Watt usage
 * but do provide current totals, feeding this regular updates will allow
 * a fair average to be calculated.
 *
 * Use case:
 *
 *   We need to get a lot of input, or else the Watt approximation makes no
 *   sense for low Wh deltas. For 550W, we'd still only get 9.17 Wh per minute.
 *   If we simply count the delta over 60s, we would oscillate between (5x) 9
 *   (540W) and (1x) 10 (600W):
 *
 *   - (3600/60)*9  = 540W
 *   - (3600/60)*10 = 600W
 *
 *   However, if we sample every second or so, you'll get 9 Wh for 59 seconds
 *   instead, which is a lot closer to the actual value.
 *
 *   - (3600/59)*9 = 549W
 *
 * Usage:
 *
 *   prod = WattGauge()
 *
 *   // Running this more often will get better averages.
 *   schedule_every_second(
 *        function() { prod.set_watthour(millis(), get_watthour()) })
 *
 *   // Running this less often will get better averages;
 *   // .. but you're free to get_watt()/reset() whenever you like;
 *   // .. and an interval of 15s is fine for higher wattage (>1500).
 *   schedule_every_x_seconds(
 *      function() { push(prod.get_power()); prod.reset(); }
 */
class WattGauge
{
private:
    long _t0;       /* first time in a series */
    long _tlast;    /* latest time */
    long _p0;       /* first watt hour in a series */
    long _plast;    /* latest watt hour (1 wh = 3600 joule) */
    unsigned _watt; /* average value, but only if it makes some sense */

    /* _tdelta can be negative when the time wraps! */
    inline long _tdelta() { return _tlast - _t0; }
    /* _pdelta should never be negative */
    inline long _pdelta() { return _plast - _p0; }

    /* Are there enough values to make any reasonable estimate?
     * - Minimum sampling interval: 20s
     * - Minimum sampling size: 6 */
    inline bool _there_are_enough_values() {
        return (
            (_tdelta() >= 20000 && _pdelta() >= 6) ||
            (_tdelta() >= 50000 && _pdelta() >= 2) ||
            (_tdelta() >= 300000));
     }

    /* Recalculate watt usage, but only if there are enough values */
    inline void _recalculate_if_sensible() {
        if (_there_are_enough_values()) {
            _watt = (_pdelta() * 1000L * 3600L / _tdelta());
        } else if (_tdelta() > 300000) {
            _watt = 0;
        }
    }

public:
    WattGauge() : _t0(-1), _watt(0) {}

    /* Get the latest stored value in watt hours */
    inline long get_energy_total() {
        return _plast;
    }

    /* Get a best guess of the current power usage in watt */
    inline unsigned get_power() {
        return _watt;
    }

    /* After reading get_power() you'll generally want to reset the
     * state to start a new measurement interval */
    inline void reset() {
        if (_there_are_enough_values()) {
            /* We don't touch the _watt average. Also note that we update to
             * the latest time-in-which-there-was-a-change. */
            _t0 = _tlast;
            _p0 = _plast;
        }
    }

    /* Feed data to the WattGauge: do this often */
    inline void set_watthour(long time_ms, long current_wh) {
        if (current_wh == _plast) {
            /* Do nothing and especially not update averages */
        } else {
            /* There was a change: update values. Only store the time
             * when there were changes. */
            _tlast = time_ms;
            _plast = current_wh;
            /* Update average values if it is sensible to do so */
            if (_t0 > 0) {
                /* Only recalculate now that we have a new value */
                _recalculate_if_sensible();
            } else /* happens only once after construction */ {
                _t0 = time_ms;
                _p0 = current_wh;
            }
        }
    }
};

#ifdef TEST_BUILD
static int STR_EQ(const char *func, const char *got, const char *expected);
static int INT_EQ(const char *func, int got, int expected);
extern "C" int printf(const char *, ...);
extern "C" int atoi(const char *) throw ();
extern "C" int strcmp(const char *s1, const char *s2) throw();

static void test_wattgauge()
{
  struct { const char *const tm; long val; } data[] = {
    // At t = 0
    {"10:10:07.264", 33130232},
    {"10:10:09.223", 33130233},
    {"10:10:11.053", 33130233},
    {"10:10:12.878", 33130233},
    {"10:10:14.708", 33130233},
    {"10:10:16.538", 33130233},
    {"10:10:18.368", 33130233},
    {"10:10:20.199", 33130233},
    {"10:10:22.029", 33130234},
    {"10:10:23.858", 33130234},
    {"10:10:25.687", 33130234},
    {"10:10:27.517", 33130234},
    {"10:10:29.347", 33130234},
    {"10:10:31.177", 33130234},
    {"10:10:33.007", 33130234},
    {"10:10:34.936", 33130235},
    {"10:10:36.766", 33130235},
    {"10:10:38.594", 33130235},
    // At t = 30: we still do not have any averages
    {"TEST", 0},
    {"10:10:40.425", 33130235},
    {"10:10:42.256", 33130235},
    {"10:10:44.086", 33130235},
    {"10:10:45.915", 33130235},
    {"10:10:47.978", 33130236},
    {"10:10:49.808", 33130236},
    {"10:10:51.637", 33130236},
    {"10:10:53.467", 33130236},
    {"10:10:55.297", 33130236},
    {"10:10:57.127", 33130236},
    {"10:10:58.958", 33130236},
    {"10:11:00.988", 33130237}, // <- plast
    {"10:11:02.819", 33130237},
    {"10:11:04.648", 33130237},
    {"10:11:06.478", 33130237},
    {"10:11:08.308", 33130237},
    // At t = 60: we have an average
    {"RESET", 335},
    {"10:11:10.138", 33130237},
    {"10:11:11.969", 33130237},
    {"10:11:14.032", 33130238},
    {"10:11:15.861", 33130238},
    {"10:11:17.692", 33130238},
    {"10:11:19.522", 33130238},
    {"10:11:21.352", 33130238},
    {"10:11:23.182", 33130238},
    {"10:11:25.013", 33130238},
    {"10:11:27.075", 33130239},
    {"10:11:28.905", 33130239},
    {"10:11:30.735", 33130239},
    {"10:11:32.565", 33130239},
    {"10:11:34.396", 33130239},
    {"10:11:36.226", 33130239},
    {"10:11:38.056", 33130239},
    {"10:11:39.886", 33130240},
    {"10:11:42.382", 33130242},
    {"10:11:44.212", 33130243},
    {"10:11:46.840", 33130245},
    {"10:11:48.670", 33130246},
    {"10:11:51.332", 33130248},
    {"10:11:53.162", 33130249},
    {"10:11:55.823", 33130251},
    {"10:11:57.653", 33130252},
    {"10:12:00.282", 33130254},
    {"10:12:02.146", 33130255},
    {"10:12:04.775", 33130257},
    {"10:12:06.604", 33130258},
    {"10:12:09.300", 33130260}, // <- plast
    // At t = 120: we have an higher average
    // 33130260 - 33130237         = 23 Wh
    // 10:12:09.300 - 10:11:00.988 = 68.312 s
    // (3600 / 68.312) * 23        = 1212 Watt
    {"RESET", 1212},
    {"10:12:11.129", 33130261},
    {"10:12:13.790", 33130263},
    {"10:12:15.619", 33130264},
    {"10:12:18.281", 33130266},
    {"10:12:20.111", 33130267},
    {"10:12:22.773", 33130269},
    {"10:12:24.602", 33130270},
    {"10:12:27.264", 33130272},
    {"10:12:29.093", 33130273},
    {"10:12:31.758", 33130275},
    {"10:12:33.588", 33130276},
    {"10:12:36.250", 33130278},
    {"10:12:38.080", 33130279},
    {"10:12:40.742", 33130281}, // <- plast
    // At t = 150: for power>=1000 we push every 30s instead
    // 33130281 - 33130260         = 21 Wh
    // 10:12:40.742 - 10:12:09.300 = 31.442 s
    // (3600 / 31.442) * 21        = 2404 Watt
    {"RESET", 2404},
    {"10:12:42.573", 33130282},
    {"10:12:45.267", 33130284},
    {"10:12:47.096", 33130285},
    {"10:12:49.823", 33130287},
    {"10:12:51.653", 33130288},
    {"10:12:54.348", 33130290},
    {"10:12:56.178", 33130291},
    {"10:12:58.873", 33130293},
    {"10:13:00.701", 33130294},
    {"10:13:03.431", 33130296},
    {"10:13:05.262", 33130297},
    {"10:13:07.958", 33130299},
    {"10:13:09.789", 33130300},
    {"10:13:12.452", 33130302},
    {"10:13:14.282", 33130303},
    {"RESET", 2361},
    {"10:13:16.944", 33130305},
    {"10:13:18.775", 33130306},
    {"10:13:21.438", 33130308},
    {"10:13:23.267", 33130309},
    {"10:13:25.964", 33130311},
    {"10:13:27.794", 33130312},
    {"10:13:30.455", 33130314},
    {"10:13:32.284", 33130315},
    {"10:13:34.957", 33130317},
    {"10:13:36.820", 33130318},
    {"10:13:39.481", 33130320},
    {"10:13:41.310", 33130321},
    {"10:13:44.003", 33130323},
    {"10:13:45.833", 33130324},
    {"RESET", 2396},
    {"10:13:48.528", 33130326},
    {"10:13:50.357", 33130327},
    {"10:13:53.018", 33130329},
    {"10:13:54.847", 33130329},
    {"10:13:56.677", 33130329},
    {"10:13:58.508", 33130329},
    {"10:14:00.438", 33130330},
    {"10:14:02.268", 33130330},
    {"10:14:04.100", 33130330},
    {"10:14:05.897", 33130330},
    {"10:14:07.759", 33130330},
    {"10:14:09.590", 33130330},
    {"10:14:11.420", 33130330},
    {"10:14:13.250", 33130331}, // <- plast
    {"10:14:15.047", 33130331},
    {"10:14:16.877", 33130331},
    {"RESET", 919},
    {"10:14:18.706", 33130331},
    {"10:14:20.536", 33130331},
    {"10:14:22.363", 33130331},
    {"10:14:24.959", 33130332},
    {"10:14:26.788", 33130332},
    {"10:14:28.618", 33130332},
    {"10:14:30.448", 33130332},
    {"10:14:32.249", 33130332},
    {"10:14:34.079", 33130332},
    {"10:14:35.910", 33130332},
    {"10:14:37.740", 33130333},
    {"10:14:39.604", 33130333},
    {"10:14:41.401", 33130333},
    {"10:14:43.231", 33130333},
    {"10:14:45.060", 33130333},
    {"10:14:46.890", 33130333},
    {"10:14:49.551", 33130334},
    {"10:14:51.380", 33130334},
    {"10:14:53.207", 33130334},
    {"10:14:55.038", 33130334},
    {"10:14:56.835", 33130334},
    {"10:14:58.665", 33130334},
    {"10:15:00.496", 33130334},
    {"10:15:02.326", 33130335},
    {"10:15:04.157", 33130335},
    {"10:15:05.988", 33130335},
    {"10:15:07.818", 33130335},
    {"10:15:09.648", 33130335},
    {"10:15:11.478", 33130335},
    {"TEST", 919}, // fewer than 50 seconds after plast
    {"10:15:14.174", 33130336},
    {"TEST", 295}, // more than 50 seconds after plast
    {"10:15:16.005", 33130336},
    {"10:15:17.835", 33130336},
    {"10:15:19.665", 33130336},
    {"RESET", 295},
    {"10:15:21.497", 33130336},
    {"10:15:23.327", 33130336},
    {"10:15:25.158", 33130336},
    {"10:15:26.988", 33130337},
    {"10:15:28.819", 33130337},
    {"10:15:30.649", 33130337},
    {"10:15:32.446", 33130337},
    {"10:15:34.277", 33130337},
    {"10:15:36.106", 33130337},
    {"10:15:38.902", 33130338},
    {"10:15:40.732", 33130338},
    {"10:15:42.561", 33130338},
    {"10:15:44.393", 33130338},
    {"10:15:46.223", 33130338},
    {"10:15:48.051", 33130338},
    {"10:15:49.880", 33130338},
    {"10:15:51.705", 33130339},
    {"10:15:53.536", 33130339},
    {"10:15:55.364", 33130339},
    {"10:15:57.194", 33130339},
    {"10:15:59.019", 33130339},
    {"10:16:00.849", 33130339},
    {"10:16:02.679", 33130339},
    {"10:16:04.510", 33130340},
    {"10:16:06.339", 33130340},
    {"10:16:08.169", 33130340},
    {"10:16:10.000", 33130340},
    {"10:16:11.830", 33130340},
    {"10:16:13.625", 33130340},
    {"10:16:16.153", 33130341},
    {"10:16:17.983", 33130341},
    {"10:16:19.810", 33130341},
    {"10:16:21.669", 33130341},
    {"10:16:23.498", 33130341},
    {"10:16:25.326", 33130341},
    {"10:16:27.155", 33130341},
    {"10:16:29.019", 33130342},
    {"10:16:30.849", 33130342},
    {"10:16:32.677", 33130342},
    {"10:16:34.507", 33130342},
    {"10:16:36.336", 33130342},
    {"10:16:38.163", 33130342},
    {"10:16:39.990", 33130342},
    {"10:16:42.117", 33130343},
    {"10:16:43.945", 33130343},
    {"10:16:45.742", 33130343},
    {"10:16:47.571", 33130343},
    {"10:16:49.400", 33130343},
    {"10:16:51.229", 33130343},
    {"10:16:53.056", 33130343},
    {"10:16:55.116", 33130344},
    {"10:16:56.945", 33130344},
    {"10:16:58.774", 33130344},
    {"10:17:00.603", 33130344},
    {"10:17:02.432", 33130344},
    {"10:17:04.302", 33130344},
    {0, 0}
  };

  WattGauge positive;
  for (int i = 0; data[i].tm; ++i) {
    const char *const tm = data[i].tm;
    if (strcmp(tm, "TEST") == 0) {
        INT_EQ("test_wattgauge(cur)", positive.get_power(), data[i].val);
    } else if (strcmp(tm, "RESET") == 0) {
        INT_EQ("test_wattgauge(push)", positive.get_power(), data[i].val);
        positive.reset();
    } else {
        long ms = (
            atoi(tm + 0) * 1000 * 3600 +
            atoi(tm + 3) * 1000 * 60 +
            atoi(tm + 6) * 1000 +
            atoi(tm + 9));
        positive.set_watthour(ms, data[i].val);
        //printf("%s: %ld Wh %u Watt\n", tm, data[i].val, positive.get_power());
    }
  }
  printf("\n");
}
#endif

// vim: set ts=8 sw=4 sts=4 et ai:
#endif //INCLUDED_WATTGAUGE_H
