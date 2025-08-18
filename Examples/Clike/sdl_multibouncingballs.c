/*
 * SDL Multi Bouncing Balls demo ported to the clike front end.
 * Requires Pscal built with SDL support.
 */

int main() {
    int WindowWidth;
    int WindowHeight;
    int TargetFPS;
    int FrameDelay;
    int NumBalls;
    float MaxInitialSpeed;
    float MinInitialSpeed;
    float Pi;
    float x[90];
    float y[90];
    float dx[90];
    float dy[90];
    int radius[90];
    int r[90];
    int g[90];
    int b[90];
    float mass[90];
    int active[90];
    int i;
    int j;
    int quit;
    int MaxX;
    int MaxY;
    float Speed_pps;
    float Angle;
    int speedRange;
    int ii;
    int jj;

    WindowWidth = 1280;
    WindowHeight = 1024;
    TargetFPS = 60;
//    FrameDelay = 1000 div TargetFPS;
    FrameDelay = trunc(1000 / TargetFPS);
//    FrameDelay = 1000 / TargetFPS;
    NumBalls = 90;
    MaxInitialSpeed = 250.0;
    MinInitialSpeed = 80.0;
    Pi = 3.14159265;

    initgraph(WindowWidth, WindowHeight, "Multi Bouncing Balls Demo");
    randomize();
    MaxX = getmaxx();
    MaxY = getmaxy();

    i = 0;
    while (i < NumBalls) {
        ii = i;
        radius[ii] = 8 + random(13);
        x[ii] = radius[ii] + random(WindowWidth - 2 * radius[ii]);
        y[ii] = radius[ii] + random(WindowHeight - 2 * radius[ii]);
        speedRange = trunc(MaxInitialSpeed - MinInitialSpeed + 1);
        Speed_pps = MinInitialSpeed + random(speedRange);
        Angle = random(360) * (Pi / 180.0);
        dx[ii] = cos(Angle) * Speed_pps / TargetFPS;
        dy[ii] = sin(Angle) * Speed_pps / TargetFPS;
        if ((abs(dx[ii]) < 0.1) && (abs(dy[ii]) < 0.1)) {
            dx[ii] = (MinInitialSpeed / TargetFPS) * 0.707;
            dy[ii] = (MinInitialSpeed / TargetFPS) * 0.707;
        }
        r[ii] = random(206) + 50;
        g[ii] = random(206) + 50;
        b[ii] = random(206) + 50;
        mass[ii] = radius[ii] * radius[ii];
        active[ii] = 1;
        i = i + 1;
    }

    quit = 0;
    printf("Multi Bouncing Balls... Press Q in the console to quit.\n");
    while (keypressed()) { readkey(); } // Clear any buffered key presses
    while (!quit) {
        if (keypressed()) {
            int c;
            c = readkey();
            if (upcase(c) == 'Q') {
                quit = 1;
            }
        }

        i = 0;
        while (i < NumBalls) {
            ii = i;
            if (active[ii]) {
                x[ii] = x[ii] + dx[ii];
                y[ii] = y[ii] + dy[ii];
                if ((x[ii] - radius[ii]) < 0) {
                    x[ii] = radius[ii];
                    dx[ii] = -dx[ii];
                } else if ((x[ii] + radius[ii]) > MaxX) {
                    x[ii] = MaxX - radius[ii];
                    dx[ii] = -dx[ii];
                }
                if ((y[ii] - radius[ii]) < 0) {
                    y[ii] = radius[ii];
                    dy[ii] = -dy[ii];
                } else if ((y[ii] + radius[ii]) > MaxY) {
                    y[ii] = MaxY - radius[ii];
                    dy[ii] = -dy[ii];
                }
                j = i + 1;
                while (j < NumBalls) {
                    jj = j;
                    if (active[jj]) {
                        float distSq;
                        float sumRadiiSq;
                        float nx;
                        float ny;
                        float dist;
                        float overlap;
                        float v1x;
                        float v1y;
                        float v2x;
                        float v2y;
                        float v1n;
                        float v1t;
                        float v2n;
                        float v2t;
                        float new_v1n;
                        float new_v2n;
                        float tx;
                        float ty;
                        float m1;
                        float m2;
                        distSq = (x[ii] - x[jj]) * (x[ii] - x[jj]) + (y[ii] - y[jj]) * (y[ii] - y[jj]);
                        sumRadiiSq = (radius[ii] + radius[jj]) * (radius[ii] + radius[jj]);
                        if (distSq <= sumRadiiSq) {
                            dist = sqrt(distSq);
                            if (dist == 0.0) {
                                x[ii] = x[ii] + (random(11) - 5) * 0.1;
                                y[jj] = y[jj] + (random(11) - 5) * 0.1;
                                dist = sqrt((x[ii] - x[jj]) * (x[ii] - x[jj]) + (y[ii] - y[jj]) * (y[ii] - y[jj]));
                                if (dist == 0.0) {
                                    dist = 0.001;
                                }
                            }
                            nx = (x[jj] - x[ii]) / dist;
                            ny = (y[jj] - y[ii]) / dist;
                            tx = -ny;
                            ty = nx;
                            v1x = dx[ii];
                            v1y = dy[ii];
                            v2x = dx[jj];
                            v2y = dy[jj];
                            v1n = v1x * nx + v1y * ny;
                            v1t = v1x * tx + v1y * ty;
                            v2n = v2x * nx + v2y * ny;
                            v2t = v2x * tx + v2y * ty;
                            m1 = mass[ii];
                            m2 = mass[jj];
                            if ((m1 + m2) == 0) {
                                new_v1n = 0;
                                new_v2n = 0;
                            } else {
                                new_v1n = (v1n * (m1 - m2) + 2 * m2 * v2n) / (m1 + m2);
                                new_v2n = (v2n * (m2 - m1) + 2 * m1 * v1n) / (m1 + m2);
                            }
                            dx[ii] = new_v1n * nx + v1t * tx;
                            dy[ii] = new_v1n * ny + v1t * ty;
                            dx[jj] = new_v2n * nx + v2t * tx;
                            dy[jj] = new_v2n * ny + v2t * ty;
                            overlap = (radius[ii] + radius[jj]) - dist;
                            if (overlap > 0.0) {
                                x[ii] = x[ii] - (overlap / 2.0) * nx;
                                y[ii] = y[ii] - (overlap / 2.0) * ny;
                                x[jj] = x[jj] + (overlap / 2.0) * nx;
                                y[jj] = y[jj] + (overlap / 2.0) * ny;
                            }
                        }
                    }
                    j = j + 1;
                }
            }
            i = i + 1;
        }
        cleardevice();
        i = 0;
        while (i < NumBalls) {
            ii = i;
            if (active[ii]) {
                setrgbcolor(r[ii], g[ii], b[ii]);
                fillcircle(trunc(x[ii]), trunc(y[ii]), radius[ii]);
            }
            i = i + 1;
        }
        updatescreen();
        graphloop(FrameDelay);
    }
    closegraph();
    printf("Demo finished.\n");
    return 0;
}
