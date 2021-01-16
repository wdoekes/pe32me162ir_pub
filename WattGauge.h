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
    float _watt;    /* average value, but only if it makes some sense */

    /* _tdelta can be negative when the time wraps! */
    inline long _tdelta() { return _tlast - _t0; }
    /* _pdelta should never be negative */
    inline long _pdelta() { return _plast - _p0; }

    /* Are there enough values to make any reasonable estimate?
     * - Minimum sampling interval: 12s
     * - Minimum sampling size: 5 */
    inline bool _there_are_enough_values() {
        return (
            (_tdelta() >= 12000 && _pdelta() >= 5) ||
            (_tdelta() >= 50000 && _pdelta() >= 2));
    }

    /* Recalculate watt usage, but only if there are enough values */
    inline void _recalculate_if_sensible() {
        if (_there_are_enough_values()) {
            _watt = (_pdelta() * 1000 * 3600 / _tdelta());
        }
    }

public:
    WattGauge() : _t0(-1), _watt(0.0) {}

    /* Get the latest stored value in watt hours */
    inline long get_energy_total() {
        return _plast;
    }

    /* Get a best guess of the current power usage in watt */
    inline float get_power() {
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

// vim: set ts=8 sw=4 sts=4 et ai:
#endif //INCLUDED_WATTGAUGE_H
