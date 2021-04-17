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
 *   schedule_every_second(function() {
 *       prod.set_active_energy_total(millis(), get_watthour()) })
 *
 *   // Running this less often will get better averages;
 *   // .. but you're free to get_watt()/reset() whenever you like;
 *   // .. and an interval of 15s is fine for higher wattage (>1500).
 *   schedule_every_x_seconds(function() {
 *       push(prod.get_instantaneous_power()); prod.reset(); }
 */
class WattGauge
{
private:
    unsigned long _t[3];  /* t0, t(end-1), t(end) */
    unsigned long _p[3];  /* P(sum) in t[n] */
    unsigned long _tlast; /* latest time, even without changed data */
    int _watt;            /* average value, but only if it makes some sense */
    bool _data_valid;     /* Are the contents of _t and _p valid? Always true
                             after first sample. */

    inline unsigned long _tdelta() { return _t[2] - _t[0]; }
    inline unsigned long _pdelta() { return _p[2] - _p[0]; }

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
            _watt = (_pdelta() * 1000UL * 3600UL / _tdelta());
        } else if ((_tlast - _t[0]) > 300000) {
            _watt = 0;
        }
    }

public:
    WattGauge() : _watt(0), _data_valid(false) {}

    /* Get the latest stored value in watt hours */
    inline unsigned long get_active_energy_total() {
        return _p[2];
    }

    /* Get a best guess of the current power usage in watt */
    inline unsigned get_instantaneous_power() {
        return _watt;
    }

    /* Is there anything report for this interval? */
    inline unsigned long interval_since_last_change() {
        return (_tlast - _t[2]);
    }

    /* Feed data to the WattGauge: do this often */
    void set_active_energy_total(
            unsigned long time_ms, unsigned long current_wh) {
        _tlast = time_ms;

        /* Happens only once after construction */
        if (!_data_valid) {
            _t[0] = _t[1] = _t[2] = time_ms;
            _p[0] = _p[1] = _p[2] = current_wh;
            _watt = 0;
            _data_valid = true;
            return;
        }

        /* If there was no change. Do nothing. */
        if (current_wh == _p[2]) {
            /* Except if there was activity earlier, but not anymore.
             * 60 W is 1 Wh/min, so let's recalculate based on the
             * latest values only. */
            if ((_tlast - _t[2]) > 30000) {
                int possible_watt = (1L * 1000L * 3600L / (_tlast - _t[2]));
                if (possible_watt < _watt) {
                    _watt = possible_watt;
                }
            }
            return;
        }

        /* Set first change */
        if (_t[0] == _t[1]) {
            _t[1] = _t[2] = time_ms;
            _p[1] = _p[2] = current_wh;
        /* Update next to last change */
        } else {
            _t[1] = _t[2];
            _p[1] = _p[2];
            _t[2] = time_ms;
            _p[2] = current_wh;
        }

        /* If the difference between the delta's is large, then
         * force a reset based on the previous value.
         * - If delta between t[0] and t[1] is more than 60 seconds;
         * - and that is only one change (1 Wh);
         * - and recent changes are 4+ times faster. */
        if ((_t[1] - _t[0]) > 60000 && (_p[1] - _p[0]) <= 1 &&
                (_t[2] - _t[1]) < 15000) {
            /* This fixes a quicker increase if usage suddenly spikes. */
            reset();
        }

        _recalculate_if_sensible();
    }

    /* After reading get_instantaneous_power() you'll generally want to reset the
     * state to start a new measurement interval */
    inline void reset() {
        if (_there_are_enough_values()) {
            /* We don't touch the _watt average. Also note that we update to
             * the latest time-in-which-there-was-a-change. */
            _t[0] = _t[1];
            _p[0] = _p[1];
            _t[1] = _t[2];
            _p[1] = _p[2];
        }
    }
};

/**
 * EnergyGauge combines two WattGauge gauges to monitor both positive
 * and negative energy.
 *
 * The combination is needed because a proper estimate for either can
 * only be given if the other is known to have a 0-delta.
 */
class EnergyGauge
{
private:
    WattGauge _positive;
    WattGauge _negative;
    int _wprev;

public:
    EnergyGauge() : _wprev(0) {};
    inline unsigned long get_positive_active_energy_total() {
        return _positive.get_active_energy_total();
    }
    inline unsigned long get_negative_active_energy_total() {
        return _negative.get_active_energy_total();
    }
    inline int get_instantaneous_power() {
        if (_positive.interval_since_last_change() <
                _negative.interval_since_last_change()) {
            return _positive.get_instantaneous_power();
        } else {
            return -_negative.get_instantaneous_power();
        }
    }
    inline bool has_significant_change() {
        int watt = get_instantaneous_power();
        if ((_wprev < 0 && watt > 0) || (watt < 0 && _wprev > 0)) {
            return true; /* sign change is significant */
        } else if (_wprev == 0 && (-20 < watt && watt < 20)) {
            return false; /* fluctuating around 0 is not significant */
        } else if (_wprev == 0) {
            return true; /* otherwise a change from 0 is significant */
        }
        float factor = (float)watt / (float)_wprev;
        if (0.6 < factor && factor < 1.6) {
            return false; /* change factor is small */
        }
        return true; /* yes, significant */
    }
    inline void set_positive_active_energy_total(
            unsigned long time_ms, unsigned long current_wh) {
        _positive.set_active_energy_total(time_ms, current_wh);
    }
    inline void set_negative_active_energy_total(
            unsigned long time_ms, unsigned long current_wh) {
        _negative.set_active_energy_total(time_ms, current_wh);
    }
    inline void reset() {
        _wprev = get_instantaneous_power();
        _positive.reset();
        _negative.reset();
    }
};

#ifdef TEST_BUILD
static int STR_EQ(const char *func, const char *got, const char *expected);
static int INT_EQ(const char *func, int got, int expected);
extern "C" int printf(const char *, ...);
extern "C" int atoi(const char *) throw ();
extern "C" int strcmp(const char *s1, const char *s2) throw();

static void _test_wattgauge()
{
  struct { const char *const tm; unsigned long val; } data[] = {
    // At t = 0
    {"10:10:07.264", 33130232}, // <- p[0]
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
    {"10:10:47.978", 33130236}, // <- p[1]
    {"10:10:49.808", 33130236},
    {"10:10:51.637", 33130236},
    {"10:10:53.467", 33130236},
    {"10:10:55.297", 33130236},
    {"10:10:57.127", 33130236},
    {"10:10:58.958", 33130236},
    {"10:11:00.988", 33130237}, // <- p[2]
    {"10:11:02.819", 33130237},
    {"10:11:04.648", 33130237},
    {"10:11:06.478", 33130237},
    {"10:11:08.308", 33130237},
    // At t = 60: we have an average
    {"RESET", 335}, // p[0] = 33130236 (prev p[1]), t[0] = 10:10:47.978
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
    {"10:12:06.604", 33130258}, // <- p[1]
    {"10:12:09.300", 33130260}, // <- p[2]
    // At t = 120: we have an higher average
    // 33130260 - 33130236         = 24 Wh = 86400 J
    // 10:12:09.300 - 10:10.47.978 = 81.312 s
    // 86400 / 81.312              = 1062 Watt
    {"RESET", 1062},
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
    {"10:12:40.742", 33130281}, // <- p[2]
    // At t = 150: for power>=1000 we push every 30s instead
    // 33130281 - 33130258         = 23 Wh = 82800 J
    // 10:12:40.742 - 10:12:06.604 = 34.138 s
    // 82800 / 34.138              = 2425 Watt
    {"RESET", 2425},
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
    {"RESET", 2386},
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
    {"RESET", 2372},
    {"10:13:48.528", 33130326},
    {"10:13:50.357", 33130327},
    {"10:13:53.018", 33130329},
    {"10:13:54.847", 33130329},
    {"10:13:56.677", 33130329},
    {"10:13:58.508", 33130329},
    {"10:14:00.438", 33130330}, // <- p[1]
    {"10:14:02.268", 33130330},
    {"10:14:04.100", 33130330},
    {"10:14:05.897", 33130330},
    {"10:14:07.759", 33130330},
    {"10:14:09.590", 33130330},
    {"10:14:11.420", 33130330},
    {"10:14:13.250", 33130331}, // <- p[2]
    {"10:14:15.047", 33130331},
    {"10:14:16.877", 33130331},
    {"RESET", 984},
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
    {"TEST", 984}, // fewer than 50 seconds after p[0]
    {"10:15:02.326", 33130335},
    {"TEST", 290}, // more than 50 seconds after p[0]
    {"10:15:04.157", 33130335},
    {"10:15:05.988", 33130335},
    {"10:15:07.818", 33130335},
    {"10:15:09.648", 33130335},
    {"10:15:11.478", 33130335},
    {"10:15:14.174", 33130336},
    {"RESET", 292},
    {"10:15:16.005", 33130336},
    {"10:15:17.835", 33130336},
    {"10:15:19.665", 33130336},
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
        INT_EQ(
            "test_wattgauge(cur)",
            positive.get_instantaneous_power(), data[i].val);
    } else if (strcmp(tm, "RESET") == 0) {
        INT_EQ(
            "test_wattgauge(push)",
            positive.get_instantaneous_power(), data[i].val);
        positive.reset();
    } else {
        unsigned long ms = (
            atoi(tm + 0) * 1000 * 3600 +
            atoi(tm + 3) * 1000 * 60 +
            atoi(tm + 6) * 1000 +
            atoi(tm + 9));
        positive.set_active_energy_total(ms, data[i].val);
#if 0
        printf("%s: %ld Wh %d Watt\n",
            tm, data[i].val, positive.get_instantaneous_power());
#endif
    }
  }
  printf("\n");
}

static void _test_energygauge()
{
  struct { const char *const tm; int val; } data[] = {
    // At t = 0
    {"HAS_CHANGE", 0},
    {"TEST", 0},
    {"19:14:24.280", 33268826}, // push after Hello
    {"HAS_CHANGE", 0},
    {"TEST", 0},
    {"19:14:26.239", 33268827},
    {"19:14:28.066", 33268827},
    {"19:14:29.892", 33268827},
    {"19:14:31.719", 33268827},
    {"19:14:33.545", 33268827},
    {"19:14:35.372", 33268827},
    {"19:14:37.231", 33268828},
    {"19:14:39.058", 33268828},
    {"19:14:40.885", 33268828},
    {"19:14:42.712", 33268828},
    {"19:14:44.538", 33268828},
    {"19:14:46.365", 33268828},
    {"19:14:48.192", 33268829},
    {"19:14:50.019", 33268829},
    {"19:14:51.850", 33268829},
    {"19:14:53.677", 33268829},
    {"19:14:55.504", 33268829},
    {"19:14:57.330", 33268829},
    {"19:14:59.157", 33268829},
    {"19:15:00.984", 33268830},
    {"19:15:02.811", 33268830},
    {"19:15:04.638", 33268830},
    {"19:15:06.465", 33268830},
    {"19:15:08.292", 33268830},
    {"19:15:10.119", 33268830},
    {"19:15:11.945", 33268831}, // <- p[1]
    {"19:15:13.775", 33268831},
    {"19:15:15.603", 33268831},
    {"19:15:17.430", 33268831},
    {"19:15:19.256", 33268831},
    {"19:15:21.082", 33268831},
    {"19:15:22.908", 33268831},
    {"HAS_CHANGE", 0},
    {"TEST", 0},
    {"19:15:24.743", 33268832}, // <- p[2]
    {"HAS_CHANGE", 1},
    {"RESET", 357},
    {"19:15:26.576", 33268832},
    {"19:15:28.412", 33268832},
    {"19:15:30.245", 33268832},
    {"19:15:32.076", 33268832},
    {"19:15:33.912", 33268832},
    {"19:15:35.713", 33268833},
    {"19:15:37.545", 33268833},
    {"19:15:39.378", 33268833},
    {"19:15:41.211", 33268833},
    {"19:15:43.044", 33268833},
    {"19:15:44.875", 33268833},
    {"19:15:46.708", 33268834},
    {"19:15:48.540", 33268834},
    {"19:15:50.370", 33268834},
    {"19:15:52.168", 33268834},
    {"19:15:53.999", 33268834},
    {"19:15:55.830", 33268834},
    {"19:15:57.662", 33268835},
    {"19:15:59.493", 33268835},
    {"19:16:01.325", 33268835},
    {"19:16:03.154", 33268835},
    {"19:16:04.984", 33268835},
    {"19:16:06.815", 33268835},
    {"19:16:08.646", 33268836},
    {"19:16:10.478", 33268836},
    {"19:16:12.308", 33268836},
    {"19:16:14.110", 33268836},
    {"19:16:15.945", 33268836},
    {"19:16:17.778", 33268836},
    {"HAS_CHANGE", 0},
    {"TEST", 317},
    {"19:16:19.610", 33268837},
    {"HAS_CHANGE", 0},
    {"TEST", 319},
    {"19:16:21.443", 33268837},
    {"19:16:23.247", 33268837},
    {"19:16:25.078", 33268837},
    {"19:16:26.910", 33268837},
    {"19:16:28.743", 33268837},
    {"19:16:30.577", 33268838},
    {"19:16:32.410", 33268838},
    {"19:16:34.242", 33268838},
    {"19:16:36.073", 33268838},
    {"19:16:37.870", 33268838},
    {"19:16:39.701", 33268839},
    {"19:16:41.532", 33268839},
    {"19:16:43.364", 33268839},
    {"19:16:45.194", 33268839},
    {"19:16:47.025", 33268839},
    {"19:16:48.856", 33268839},
    {"19:16:50.686", 33268840},
    {"19:16:52.516", 33268840},
    {"19:16:54.347", 33268840},
    {"19:16:56.177", 33268840},
    {"19:16:58.007", 33268840},
    {"19:16:59.836", 33268840},
    {"19:17:01.633", 33268841},
    {"19:17:03.462", 33268841},
    {"19:17:05.293", 33268841},
    {"19:17:07.122", 33268841},
    {"19:17:08.954", 33268841},
    {"19:17:10.785", 33268841},
    {"19:17:12.616", 33268842},
    {"19:17:14.446", 33268842},
    {"19:17:16.276", 33268842},
    {"19:17:18.112", 33268842},
    {"19:17:19.944", 33268842},
    {"19:17:21.774", 33268842},
    {"19:17:23.572", 33268843}, // <- p[2]
    {"19:17:25.403", 33268843},
    {"RESET", 328}, // dP = 12 Wh = 43200 J, dT 131.627s, P = 328
    {"19:17:27.235", 33268843},
    {"19:17:29.066", 33268843},
    {"19:17:30.896", 33268843},
    {"19:17:32.727", 33268843},
    {"19:17:34.558", 33268844},
    {"19:17:36.385", 33268844},
    {"19:17:38.217", 33268844},
    {"19:17:40.048", 33268844},
    {"19:17:41.878", 33268844},
    {"19:17:43.708", 33268844},
    {"19:17:45.509", 33268845},
    {"19:17:47.343", 33268845},
    {"19:17:49.175", 33268846},
    {"19:17:51.006", 33268846},
    {"HAS_CHANGE", 0},
    {"19:17:52.836", 33268848},
    {"HAS_CHANGE", 1},
    {"RESET", 537},
    {"19:17:54.665", 33268849},
    {"19:17:56.496", 33268850},
    {"19:17:58.326", 33268852},
    {"19:18:00.157", 33268853},
    {"19:18:01.988", 33268854},
    {"19:18:03.819", 33268855},
    {"19:18:05.649", 33268857},
    {"19:18:07.481", 33268858},
    {"HAS_CHANGE", 0},
    {"19:18:09.313", 33268859},
    {"HAS_CHANGE", 1},
    {"TEST", 2323},
    {"19:18:11.145", 33268861},
    {"19:18:12.944", 33268861},
    {"19:18:14.774", 33268863},
    {"19:18:16.609", 33268864},
    {"19:18:18.438", 33268865},
    {"19:18:20.268", 33268867},
    {"RESET", 2431},
    {"19:18:22.098", 33268868},
    {"19:18:23.929", 33268869},
    {"19:18:25.760", 33268870},
    {"19:18:27.591", 33268872},
    {"19:18:29.422", 33268873},
    {"19:18:31.250", 33268874},
    {"19:18:33.080", 33268876},
    {"19:18:34.910", 33268876},
    {"19:18:36.741", 33268878},
    {"19:18:38.572", 33268879},
    {"19:18:40.400", 33268881},
    {"19:18:42.228", 33268882},
    {"19:18:44.057", 33268883},
    {"19:18:45.888", 33268884},
    {"19:18:47.685", 33268885},
    {"19:18:49.513", 33268887},
    {"19:18:51.342", 33268888},
    {"19:18:53.173", 33268889},
    {"HAS_CHANGE", 0},
    {"TEST", 2487},
    {0, 0}
  };

  EnergyGauge gauge;
  for (int i = 0; data[i].tm; ++i) {
    const char *const tm = data[i].tm;
    if (strcmp(tm, "TEST") == 0) {
        INT_EQ(
            "test_energygauge(cur)",
            gauge.get_instantaneous_power(), data[i].val);
    } else if (strcmp(tm, "HAS_CHANGE") == 0) {
        INT_EQ(
            "test_energygauge(has-significant-change)",
            gauge.has_significant_change(), data[i].val);
    } else if (strcmp(tm, "RESET") == 0) {
        INT_EQ(
            "test_energygauge(push)",
            gauge.get_instantaneous_power(), data[i].val);
        gauge.reset();
    } else {
        unsigned long ms = (
            atoi(tm + 0) * 1000 * 3600 +
            atoi(tm + 3) * 1000 * 60 +
            atoi(tm + 6) * 1000 +
            atoi(tm + 9));
        gauge.set_positive_active_energy_total(ms, data[i].val);
        gauge.set_negative_active_energy_total(ms, 7784);
#if 0
        printf("%s: %ld Wh %d Watt (has-significant-change=%s)\n",
            tm, data[i].val, gauge.get_instantaneous_power(),
            (gauge.has_significant_change() ? "YES" : "no"));
#endif
    }
  }

  printf("\n");
}

static void _test_energygauge_around_zero()
{
  struct { const char *const tm; bool pos; int val; } data[] = {
    // At t = 0
    {"14:44:57.177", true, 33378152},
    {"14:44:57.477", false, 12865},
    {"HAS_CHANGE", true, 0},
    {"TEST", true, 0},
    {"14:47:41.665", true, 33378152},
    {"14:47:41.998", false, 12865},
    {"HAS_CHANGE", true, 0},
    {"TEST", true, 0},
    {"14:47:43.498", true, 33378152},
    {"14:47:43.832", false, 12866}, // <- p0 = p1
    // ...
    {"14:48:09.113", true, 33378152},
    {"14:48:09.413", false, 12866},
    {"14:48:10.946", true, 33378153}, // <- p0 = p1
    {"14:48:11.246", false, 12866},
    {"HAS_CHANGE", true, 0},
    {"TEST", true, 0},
    {"14:48:12.780", true, 33378154},
    {"14:48:13.080", false, 12866},
    {"14:48:14.614", true, 33378155},
    {"14:48:14.914", false, 12866},
    {"14:48:16.414", true, 33378156},
    {"14:48:16.748", false, 12866},
    {"14:48:18.249", true, 33378157},
    {"14:48:18.583", false, 12866},
    {"14:48:20.083", true, 33378158},
    {"14:48:20.383", false, 12866},
    {"14:48:21.914", true, 33378160},
    {"14:48:22.214", false, 12866},
    {"14:48:23.747", true, 33378160},
    {"14:48:24.047", false, 12866},
    {"14:48:25.580", true, 33378161},
    {"14:48:25.880", false, 12866},
    {"14:48:27.381", true, 33378162},
    {"14:48:27.714", false, 12866},
    {"14:48:29.213", true, 33378164},
    {"14:48:29.547", false, 12866},
    {"HAS_CHANGE", true, 0},
    {"TEST", true, 0},
    {"14:48:31.047", true, 33378165},
    {"14:48:31.347", false, 12866},
    {"HAS_CHANGE", true, 1},
    {"RESET", true, 2149}, // dP = 12 Wh = 43200 J, dT = 20.101s, P = 2149 J/s
    {"14:48:32.880", true, 33378165},
    {"14:48:33.181", false, 12866},
    // ...
    {"14:48:45.678", true, 33378165},
    {"14:48:46.011", false, 12866},
    {"14:48:47.511", true, 33378165},
    {"14:48:47.811", false, 12866},
    {"HAS_CHANGE", true, 0},
    {"TEST", true, 2149},
    {"14:48:49.344", true, 33378165},
    {"14:48:49.644", false, 12866},
    {"14:48:51.178", true, 33378165},
    {"14:48:51.478", false, 12866},
    {"14:48:53.011", true, 33378165},
    {"14:48:53.311", false, 12866},
    {"14:48:54.812", true, 33378165},
    {"14:48:55.145", false, 12866},
    {"14:48:56.646", true, 33378165},
    {"14:48:56.946", false, 12866},
    {"14:48:58.479", true, 33378165},
    {"14:48:58.780", false, 12866},
    {"14:49:00.315", true, 33378165},
    {"14:49:00.615", false, 12866},
    {"HAS_CHANGE", true, 0},
    {"TEST", true, 2149},
    {"14:49:02.148", true, 33378165},
    {"14:49:02.448", false, 12866},
    {"HAS_CHANGE", true, 1},
    {"RESET", true, 115},
    {"14:49:03.948", true, 33378165},
    {"14:49:04.281", false, 12866},
    {"14:49:05.781", true, 33378165},
    {"14:49:06.115", false, 12866},
    {"14:49:07.615", true, 33378165},
    {"14:49:07.915", false, 12866},
    {"14:49:09.451", true, 33378165},
    {"14:49:09.751", false, 12866},
    {"14:49:11.285", true, 33378165},
    {"14:49:11.585", false, 12866},
    {"14:49:13.086", true, 33378165},
    {"14:49:13.420", false, 12866},
    {"14:49:14.920", true, 33378165},
    {"14:49:15.256", false, 12866},
    {"14:49:16.756", true, 33378165},
    {"14:49:17.056", false, 12866},
    {"14:49:18.591", true, 33378165},
    {"14:49:18.891", false, 12866},
    {"14:49:20.425", true, 33378165},
    {"14:49:20.725", false, 12866},
    {"14:49:22.227", true, 33378165},
    {"14:49:22.560", false, 12866},
    {"14:49:24.061", true, 33378165},
    {"14:49:24.394", false, 12866},
    {"14:49:25.894", true, 33378165},
    {"14:49:26.194", false, 12866},
    {"14:49:27.729", true, 33378165},
    {"HAS_CHANGE", true, 1},
    {"TEST", true, 63},
    {"14:49:28.028", false, 12867},
    {"HAS_CHANGE", true, 1},
    {"RESET", true, -26},
    {"14:49:29.562", true, 33378165},
    {"14:49:29.862", false, 12867},
    {"14:49:31.396", true, 33378165},
    {"14:49:31.696", false, 12867},
    {"14:49:33.230", true, 33378165},
    {"14:49:33.530", false, 12867},
    {"14:49:35.030", true, 33378165},
    {"14:49:35.364", false, 12867},
    {"14:49:36.864", true, 33378165},
    {"14:49:37.198", false, 12867},
    {"14:49:38.699", true, 33378165},
    {"14:49:38.999", false, 12867},
    {"14:49:40.534", true, 33378165},
    {"14:49:40.834", false, 12867},
    {"14:49:42.369", true, 33378165},
    {"14:49:42.669", false, 12867},
    {"14:49:44.202", true, 33378165},
    {"14:49:44.502", false, 12867},
    {"14:49:46.002", true, 33378165},
    {"14:49:46.336", false, 12867},
    {"14:49:47.839", true, 33378165},
    {"14:49:48.139", false, 12867},
    {"14:49:49.674", true, 33378165},
    {"14:49:49.975", false, 12867},
    {"14:49:51.511", true, 33378165},
    {"14:49:51.812", false, 12867},
    {"14:49:53.312", true, 33378165},
    {"14:49:53.646", false, 12867},
    {"14:49:55.146", true, 33378165},
    {"14:49:55.479", false, 12867},
    {"14:49:56.980", true, 33378165},
    {"14:49:57.280", false, 12867},
    {"14:49:58.815", true, 33378165},
    {"14:49:59.115", false, 12867},
    {"14:50:00.650", true, 33378165},
    {"14:50:00.951", false, 12867},
    {"14:50:02.454", true, 33378165},
    {"14:50:02.787", false, 12867},
    {"14:50:04.290", true, 33378165},
    {"14:50:04.590", false, 12867},
    {"HAS_CHANGE", true, 0},
    {"14:50:06.125", true, 33378165},
    {"14:50:06.425", false, 12868},
    {"HAS_CHANGE", true, 1},
    {"RESET", true, -50},
    {0, true, 0}
  };

  EnergyGauge gauge;
  for (int i = 0; data[i].tm; ++i) {
    const char *const tm = data[i].tm;
    if (strcmp(tm, "TEST") == 0) {
        INT_EQ(
            "test_energygauge_around_zero(cur)",
            gauge.get_instantaneous_power(), data[i].val);
    } else if (strcmp(tm, "HAS_CHANGE") == 0) {
        INT_EQ(
            "test_energygauge_around_zero(has-significant-change)",
            gauge.has_significant_change(), data[i].val);
    } else if (strcmp(tm, "RESET") == 0) {
        INT_EQ(
            "test_energygauge_around_zero(push)",
            gauge.get_instantaneous_power(), data[i].val);
        gauge.reset();
    } else {
        unsigned long ms = (
            atoi(tm + 0) * 1000 * 3600 +
            atoi(tm + 3) * 1000 * 60 +
            atoi(tm + 6) * 1000 +
            atoi(tm + 9));
        if (data[i].pos) {
            gauge.set_positive_active_energy_total(ms, data[i].val);
        } else {
            gauge.set_negative_active_energy_total(ms, data[i].val);
        }
#if 0
        printf("%s: %ld Wh %d Watt (has-significant-change=%s)\n",
            tm, data[i].val, gauge.get_instantaneous_power(),
            (gauge.has_significant_change() ? "YES" : "no"));
#endif
    }
  }

  printf("\n");
}

static void test_wattgauge()
{
    _test_wattgauge();
    _test_energygauge();
    _test_energygauge_around_zero();
}
#endif

// vim: set ts=8 sw=4 sts=4 et ai:
#endif //INCLUDED_WATTGAUGE_H
