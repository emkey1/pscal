// CLike equivalent of the Rea CRT module.

int CRT_BLACK() { return 0; }
int CRT_BLUE() { return 1; }
int CRT_GREEN() { return 2; }
int CRT_CYAN() { return 3; }
int CRT_RED() { return 4; }
int CRT_MAGENTA() { return 5; }
int CRT_BROWN() { return 6; }
int CRT_LIGHT_GRAY() { return 7; }
int CRT_DARK_GRAY() { return 8; }
int CRT_LIGHT_BLUE() { return 9; }
int CRT_LIGHT_GREEN() { return 10; }
int CRT_LIGHT_CYAN() { return 11; }
int CRT_LIGHT_RED() { return 12; }
int CRT_LIGHT_MAGENTA() { return 13; }
int CRT_YELLOW() { return 14; }
int CRT_WHITE() { return 15; }
int CRT_BLINK() { return 128; }

int CRT_currentTextAttr = 7;

int CRT_normalizeTextAttr(int attr) {
    int normalized = attr % 256;
    if (normalized < 0) {
        normalized = normalized + 256;
    }
    return normalized;
}

void CRT_applyTextAttrToTerminal(int attr) {
    int foreground = attr % 16;
    int background = (attr / 16) % 8;
    int blink = attr / 128;

    normvideo();
    textcolor(foreground);
    textbackground(background);
    if (blink != 0) {
        blinktext();
    }
}

int CRT_getTextAttr() {
    return CRT_currentTextAttr;
}

void CRT_setTextAttr(int attr) {
    int normalized = CRT_normalizeTextAttr(attr);
    CRT_currentTextAttr = normalized;
    CRT_applyTextAttrToTerminal(normalized);
}
