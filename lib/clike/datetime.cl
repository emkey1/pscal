// Date/time helpers mirroring lib/rea/datetime.

str datetime_pad2(int value) {
    if (value < 10 && value >= 0) {
        return "0" + inttostr(value);
    }
    return inttostr(value);
}

int datetime_divInt(int numerator, int denominator) {
    if (denominator == 0) {
        return 0;
    }
    float n = numerator * 1.0;
    float d = denominator * 1.0;
    float q = n / d;
    int truncated = toint(q);
    if (q < 0.0 && q != truncated) {
        return truncated - 1;
    }
    return truncated;
}

str datetime_formatUtcOffset(int offsetSeconds) {
    int absSeconds = offsetSeconds;
    str sign = "+";
    if (absSeconds < 0) {
        sign = "-";
        absSeconds = -absSeconds;
    }
    int hours = datetime_divInt(absSeconds, 3600);
    int minutes = datetime_divInt(absSeconds % 3600, 60);
    return sign + datetime_pad2(hours) + ":" + datetime_pad2(minutes);
}

str datetime_formatUnix(int epochSeconds, int offsetSeconds) {
    int total = epochSeconds + offsetSeconds;
    if (total < 0) {
        total = 0;
    }
    int days = datetime_divInt(total, 86400);
    int secondsOfDay = total % 86400;
    int hour = datetime_divInt(secondsOfDay, 3600);
    int minute = datetime_divInt(secondsOfDay % 3600, 60);
    int second = secondsOfDay % 60;

    int z = days + 719468;
    int era;
    if (z >= 0) {
        era = datetime_divInt(z, 146097);
    } else {
        era = datetime_divInt(z - 146096, 146097);
    }
    int doe = z - era * 146097;
    int yoe = datetime_divInt(doe - datetime_divInt(doe, 1460) + datetime_divInt(doe, 36524) - datetime_divInt(doe, 146096), 365);
    int year = yoe + era * 400;
    int doy = doe - (365 * yoe + datetime_divInt(yoe, 4) - datetime_divInt(yoe, 100));
    int mp = datetime_divInt(5 * doy + 2, 153);
    int day = doy - datetime_divInt(153 * mp + 2, 5) + 1;
    int month;
    if (mp < 10) {
        month = mp + 3;
    } else {
        month = mp - 9;
    }
    if (month <= 2) {
        year = year + 1;
    }

    str date = inttostr(year) + "-" + datetime_pad2(month) + "-" + datetime_pad2(day);
    str time = datetime_pad2(hour) + ":" + datetime_pad2(minute) + ":" + datetime_pad2(second);
    return date + " " + time;
}

str datetime_iso8601(int epochSeconds, int offsetSeconds) {
    int total = epochSeconds + offsetSeconds;
    if (total < 0) {
        total = 0;
    }
    int days = datetime_divInt(total, 86400);
    int secondsOfDay = total % 86400;
    int hour = datetime_divInt(secondsOfDay, 3600);
    int minute = datetime_divInt(secondsOfDay % 3600, 60);
    int second = secondsOfDay % 60;

    int z = days + 719468;
    int era;
    if (z >= 0) {
        era = datetime_divInt(z, 146097);
    } else {
        era = datetime_divInt(z - 146096, 146097);
    }
    int doe = z - era * 146097;
    int yoe = datetime_divInt(doe - datetime_divInt(doe, 1460) + datetime_divInt(doe, 36524) - datetime_divInt(doe, 146096), 365);
    int year = yoe + era * 400;
    int doy = doe - (365 * yoe + datetime_divInt(yoe, 4) - datetime_divInt(yoe, 100));
    int mp = datetime_divInt(5 * doy + 2, 153);
    int day = doy - datetime_divInt(153 * mp + 2, 5) + 1;
    int month;
    if (mp < 10) {
        month = mp + 3;
    } else {
        month = mp - 9;
    }
    if (month <= 2) {
        year = year + 1;
    }

    str date = inttostr(year) + "-" + datetime_pad2(month) + "-" + datetime_pad2(day);
    str time = datetime_pad2(hour) + ":" + datetime_pad2(minute) + ":" + datetime_pad2(second);
    return date + "T" + time + datetime_formatUtcOffset(offsetSeconds);
}

int datetime_startOfDay(int epochSeconds, int offsetSeconds) {
    int total = epochSeconds + offsetSeconds;
    int days = datetime_divInt(total, 86400);
    return days * 86400 - offsetSeconds;
}

int datetime_endOfDay(int epochSeconds, int offsetSeconds) {
    int start = datetime_startOfDay(epochSeconds, offsetSeconds);
    return start + 86399;
}

int datetime_addDays(int epochSeconds, int days) {
    return epochSeconds + days * 86400;
}

int datetime_addHours(int epochSeconds, int hours) {
    return epochSeconds + hours * 3600;
}

int datetime_addMinutes(int epochSeconds, int minutes) {
    return epochSeconds + minutes * 60;
}

int datetime_daysBetween(int startEpoch, int endEpoch) {
    int diff = endEpoch - startEpoch;
    if (diff >= 0) {
        return datetime_divInt(diff, 86400);
    }
    int positive = -diff;
    return -datetime_divInt(positive + 86399, 86400);
}

str datetime_describeDifference(int startEpoch, int endEpoch) {
    int diff = endEpoch - startEpoch;
    int sign = 1;
    if (diff < 0) {
        sign = -1;
        diff = -diff;
    }
    int days = datetime_divInt(diff, 86400);
    int remainder = diff % 86400;
    int hours = datetime_divInt(remainder, 3600);
    remainder = remainder % 3600;
    int minutes = datetime_divInt(remainder, 60);
    int seconds = remainder % 60;

    str buffer = "";
    if (days > 0) {
        buffer = buffer + inttostr(days) + "d";
    }
    if (hours > 0 || (days > 0 && (minutes > 0 || seconds > 0))) {
        if (length(buffer) > 0) {
            buffer = buffer + " ";
        }
        buffer = buffer + inttostr(hours) + "h";
    }
    if (minutes > 0 || (length(buffer) > 0 && seconds > 0)) {
        if (length(buffer) > 0) {
            buffer = buffer + " ";
        }
        buffer = buffer + inttostr(minutes) + "m";
    }
    if (seconds > 0 || length(buffer) == 0) {
        if (length(buffer) > 0) {
            buffer = buffer + " ";
        }
        buffer = buffer + inttostr(seconds) + "s";
    }
    if (sign < 0) {
        buffer = "-" + buffer;
    }
    return buffer;
}
