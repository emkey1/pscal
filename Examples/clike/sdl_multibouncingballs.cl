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
    /*
     * The Pscal VM uses Pascal-style 1-based array indexing.  To mirror the
     * Pascal version of this demo we allocate one extra element for each
     * array and ignore index 0.  All loops therefore run from 1..NumBalls.
     */
    float x[91];
    float y[91];
    float dx[91];
    float dy[91];
    int radius[91];
    int r[91];
    int g[91];
    int b[91];
    float mass[91];
    int active[91];
    int i;
    int j;
    int quit;
    int MaxX;
    int MaxY;
    float Speed_pps;
    float Angle;
    int speedRange;

    WindowWidth = 1280;
    WindowHeight = 1024;
    TargetFPS = 60;
    FrameDelay = trunc(1000 / TargetFPS);
    NumBalls = 90;
    MaxInitialSpeed = 250.0;
    MinInitialSpeed = 80.0;
    Pi = 3.14159265;

    initgraph(WindowWidth, WindowHeight, "Multi Bouncing Balls Demo");
    randomize();
    MaxX = getmaxx();
    MaxY = getmaxy();

    i = 1;
    while (i <= NumBalls) {
        radius[i] = 8 + random(13);
        x[i] = radius[i] + random(WindowWidth - 2 * radius[i]);
        y[i] = radius[i] + random(WindowHeight - 2 * radius[i]);
        speedRange = trunc(MaxInitialSpeed - MinInitialSpeed + 1);
        Speed_pps = MinInitialSpeed + random(speedRange);
        Angle = random(360) * (Pi / 180.0);
        dx[i] = cos(Angle) * Speed_pps / TargetFPS;
        dy[i] = sin(Angle) * Speed_pps / TargetFPS;
        if ((abs(dx[i]) < 0.1) && (abs(dy[i]) < 0.1)) {
            dx[i] = (MinInitialSpeed / TargetFPS) * 0.707;
            dy[i] = (MinInitialSpeed / TargetFPS) * 0.707;
        }
        r[i] = random(206) + 50;
        g[i] = random(206) + 50;
        b[i] = random(206) + 50;
        mass[i] = radius[i] * radius[i];
        active[i] = 1;
        i = i + 1;
    }

    quit = 0;
    printf("Multi Bouncing Balls... Press Q in the console to quit.\n");
    while (!quit) {
        if (keypressed()) {
            int c;
            c = readkey();
            if (upcase(c) == 'Q') {
                quit = 1;
            }
        }

        i = 1;
        while (i <= NumBalls) {
            if (active[i]) {
                x[i] = x[i] + dx[i];
                y[i] = y[i] + dy[i];
                if ((x[i] - radius[i]) < 0) {
                    x[i] = radius[i];
                    dx[i] = -dx[i];
                } else if ((x[i] + radius[i]) > MaxX) {
                    x[i] = MaxX - radius[i];
                    dx[i] = -dx[i];
                }
                if ((y[i] - radius[i]) < 0) {
                    y[i] = radius[i];
                    dy[i] = -dy[i];
                } else if ((y[i] + radius[i]) > MaxY) {
                    y[i] = MaxY - radius[i];
                    dy[i] = -dy[i];
                }
                j = i + 1;
                while (j <= NumBalls) {
                    if (active[j]) {
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
                        distSq = (x[i] - x[j]) * (x[i] - x[j]) + (y[i] - y[j]) * (y[i] - y[j]);
                        sumRadiiSq = (radius[i] + radius[j]) * (radius[i] + radius[j]);
                        if (distSq <= sumRadiiSq) {
                            dist = sqrt(distSq);
                            if (dist == 0.0) {
                                x[i] = x[i] + (random(11) - 5) * 0.1;
                                y[j] = y[j] + (random(11) - 5) * 0.1;
                                dist = sqrt((x[i] - x[j]) * (x[i] - x[j]) + (y[i] - y[j]) * (y[i] - y[j]));
                                if (dist == 0.0) {
                                    dist = 0.001;
                                }
                            }
                            nx = (x[j] - x[i]) / dist;
                            ny = (y[j] - y[i]) / dist;
                            tx = -ny;
                            ty = nx;
                            v1x = dx[i];
                            v1y = dy[i];
                            v2x = dx[j];
                            v2y = dy[j];
                            v1n = v1x * nx + v1y * ny;
                            v1t = v1x * tx + v1y * ty;
                            v2n = v2x * nx + v2y * ny;
                            v2t = v2x * tx + v2y * ty;
                            m1 = mass[i];
                            m2 = mass[j];
                            if ((m1 + m2) == 0) {
                                new_v1n = 0;
                                new_v2n = 0;
                            } else {
                                new_v1n = (v1n * (m1 - m2) + 2 * m2 * v2n) / (m1 + m2);
                                new_v2n = (v2n * (m2 - m1) + 2 * m1 * v1n) / (m1 + m2);
                            }
                            dx[i] = new_v1n * nx + v1t * tx;
                            dy[i] = new_v1n * ny + v1t * ty;
                            dx[j] = new_v2n * nx + v2t * tx;
                            dy[j] = new_v2n * ny + v2t * ty;
                            overlap = (radius[i] + radius[j]) - dist;
                            if (overlap > 0.0) {
                                x[i] = x[i] - (overlap / 2.0) * nx;
                                y[i] = y[i] - (overlap / 2.0) * ny;
                                x[j] = x[j] + (overlap / 2.0) * nx;
                                y[j] = y[j] + (overlap / 2.0) * ny;
                            }
                        }
                    }
                    j = j + 1;
                }
            }
            i = i + 1;
        }
        cleardevice();
        i = 1;
        while (i <= NumBalls) {
            if (active[i]) {
                setrgbcolor(r[i], g[i], b[i]);
                fillcircle(trunc(x[i]), trunc(y[i]), radius[i]);
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
