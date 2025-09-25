# Tutorial: Writing a Rea Front End

The Rea compiler is an experimental front end for PSCAL. This tutorial walks through
building the compiler, running the SDL multi bouncing balls sample located at
`Examples/rea/sdl_multibouncingballs`, and understanding every part of the
program.

## Build the compiler

1. Configure the project and enable SDL support:
   ```sh
   cmake -B build -DSDL=ON
   ```
   This generates build files in the `build` directory and turns on the SDL
   graphics libraries required by the sample.
2. Compile the `rea` target:
   ```sh
   cmake --build build --target rea
   ```
   The resulting executable is placed at `build/bin/rea`.

## Install the PSCAL suite

The helper script installs the virtual machine and tools so the new front end can
run:
```sh
sudo ./install.sh
```

## Run the sample

For headless environments, route SDL output to dummy drivers and allow SDL usage
in the test harness:
```sh
export SDL_VIDEODRIVER=dummy
export SDL_AUDIODRIVER=dummy
export RUN_SDL=1
```
Invoke the compiler with the sample source:
```sh
build/bin/rea Examples/rea/sdl_multibouncingballs
```

## Sample walkthrough

The program displays multiple balls that bounce and collide inside a window.
Below is a section‑by‑section explanation of the source.

### `Ball` class

#### Fields
Each `Ball` instance tracks its position, velocity, and appearance:
- `x`, `y` – current coordinates
- `dx`, `dy` – velocity in pixels per frame
- `mass` – used for collision response
- `radius` – size of the drawn circle
- `r`, `g`, `b` – color components
- `active` – flag used by the main loop

**Syntax notes:** Fields inside a `class` are declared with `<type> <name>;`, and every statement is terminated with a semicolon.

#### `init`
```rea
Ball init(int w, int h, float minSpeed, float maxSpeed) {
  my.radius = 8 + random(13);                   // Note that 'my' and 'myself' are interchangeable
  my.x = my.radius + random(w - 2 * my.radius);
  my.y = my.radius + random(h - 2 * my.radius);
  int speedRange = trunc(maxSpeed - minSpeed + 1);
  float speed = minSpeed + random(speedRange);
  float angle = random(360) * (3.14159265 / 180.0);
  my.dx = cos(angle) * speed / 60.0;
  my.dy = sin(angle) * speed / 60.0;
  if ((abs(my.dx) < 0.1) && (abs(my.dy) < 0.1)) {
    my.dx = (minSpeed / 60.0) * 0.707;
    my.dy = (minSpeed / 60.0) * 0.707;
  }
  my.r = random(206) + 50;
  my.g = random(206) + 50;
  my.b = random(206) + 50;
  my.mass = my.radius * my.radius;
  my.active = true;
  return my;
}
```
- Randomizes radius and position so the ball starts fully inside the window.
- Chooses a random speed and direction; dividing by `60.0` converts pixels per
  second to per frame.
- Ensures a minimum speed so balls do not remain stationary.
- Picks a bright RGB color and computes mass as area.

**Syntax notes:** The method declaration begins with the return type `Ball` followed by the method name and typed parameters. `my` refers to the current instance. You can use `my` and `myself` interchageably.  They are identical in functionality.  

Local variables such as `int speedRange` specify the type before the name, and `if` statements use parentheses around conditions and braces for blocks.

#### `move`
```rea
void move(int maxX, int maxY) {
  my.x = my.x + my.dx;
  my.y = my.y + my.dy;
  if ((my.x - my.radius) < 0) {
    my.x = my.radius;
    my.dx = -my.dx;
  } else if ((my.x + my.radius) > maxX) {
    my.x = maxX - my.radius;
    my.dx = -my.dx;
  }
  if ((my.y - my.radius) < 0) {
    my.y = my.radius;
    my.dy = -my.dy;
  } else if ((my.y + my.radius) > maxY) {
    my.y = maxY - my.radius;
    my.dy = -my.dy;
  }
}
```
Updates position and reflects velocity when the ball hits a window edge.

**Syntax notes:** Arithmetic operators like `+` and `-` update fields, while `if`/`else if` branches control flow based on comparisons such as `<` and `>`.

#### `draw`
```rea
void draw() {
  setrgbcolor(my.r, my.g, my.b);
  fillcircle(trunc(my.x), trunc(my.y), my.radius);
}
```
Sets the draw color and renders the circle at the current location.

**Syntax notes:** Function calls like `setrgbcolor` pass arguments in parentheses, and `void` indicates the method returns no value.

### `BallsApp` class

#### Constants and fields
```rea
const int WindowWidth = 1280;
const int WindowHeight = 1024;
const int TargetFPS = 60;
const int NumBalls = 90;
const float MaxInitialSpeed = 250.0;
const float MinInitialSpeed = 80.0;

int FrameDelay;
Ball balls[NumBalls + 1];
int maxX;
int maxY;
bool quit;
```
- Constants describe the window size, frame rate, number of balls, and speed
  range.
- `FrameDelay` stores the milliseconds between frames.
- `balls` holds all ball objects. Arrays in Rea are 0‑indexed; this sample starts from index `1` and allocates an extra slot for convenience.
- `maxX` and `maxY` are the current drawable area.
- `quit` tracks when the main loop should exit.

**Syntax notes:** `const` prefixes immutable values. Arrays use square brackets with a size expression, and multiple variables of the same type can be declared sequentially.

#### `init`
```rea
void init() {
  initgraph(WindowWidth, WindowHeight, "Multi Bouncing Balls in Rea");
  randomize();
  my.maxX = getmaxx();
  my.maxY = getmaxy();
  my.FrameDelay = trunc(1000 / TargetFPS);
  int i = 1;
  while (i <= NumBalls) {
    Ball b = new Ball();
    my.balls[i] = b.init(WindowWidth, WindowHeight, MinInitialSpeed, MaxInitialSpeed);
    i = i + 1;
  }
  my.quit = false;
}
```
Initializes SDL, determines window bounds, computes the frame delay and
instantiates each ball with random settings.

**Syntax notes:** Object creation uses `new Type()`. The `while` loop runs while a condition is true, and `my.field` accesses a member of the current object.

#### `handleCollisions`
Performs pairwise collision detection and response:
```rea
void handleCollisions() {
  int i = 1;
  while (i <= NumBalls) {
    if (my.balls[i].active) {
      int j = i + 1;
      while (j <= NumBalls) {
        if (my.balls[j].active) {
          float distSq = (my.balls[i].x - my.balls[j].x) * (my.balls[i].x - my.balls[j].x) +
                         (my.balls[i].y - my.balls[j].y) * (my.balls[i].y - my.balls[j].y);
          float sumR = my.balls[i].radius + my.balls[j].radius;
          float sumR2 = sumR * sumR;
          if (distSq <= sumR2) {
            float dist = sqrt(distSq);
            if (dist == 0.0) dist = 0.001;
            float nx = (my.balls[j].x - my.balls[i].x) / dist;
            float ny = (my.balls[j].y - my.balls[i].y) / dist;
            float tx = -ny;
            float ty = nx;
            float v1x = my.balls[i].dx;
            float v1y = my.balls[i].dy;
            float v2x = my.balls[j].dx;
            float v2y = my.balls[j].dy;
            float v1n = v1x * nx + v1y * ny;
            float v1t = v1x * tx + v1y * ty;
            float v2n = v2x * nx + v2y * ny;
            float v2t = v2x * tx + v2y * ty;
            float m1 = my.balls[i].mass;
            float m2 = my.balls[j].mass;
            float new_v1n = (v1n * (m1 - m2) + 2 * m2 * v2n) / (m1 + m2);
            float new_v2n = (v2n * (m2 - m1) + 2 * m1 * v1n) / (m1 + m2);
            my.balls[i].dx = new_v1n * nx + v1t * tx;
            my.balls[i].dy = new_v1n * ny + v1t * ty;
            my.balls[j].dx = new_v2n * nx + v2t * tx;
            my.balls[j].dy = new_v2n * ny + v2t * ty;
            float overlap = sumR - dist;
            if (overlap > 0.0) {
              my.balls[i].x = my.balls[i].x - (overlap / 2.0) * nx;
              my.balls[i].y = my.balls[i].y - (overlap / 2.0) * ny;
              my.balls[j].x = my.balls[j].x + (overlap / 2.0) * nx;
              my.balls[j].y = my.balls[j].y + (overlap / 2.0) * ny;
            }
          }
        }
        j = j + 1;
      }
    }
    i = i + 1;
  }
}
```
Calculates the distance between each pair of balls, determines whether they
overlap, swaps their velocities along the collision normal, and separates them
if needed to prevent sticking.

**Syntax notes:** Nested `while` loops allow iteration over pairs. Mathematical functions like `sqrt` are called in the same way as built‑ins in C, and temporary variables are typed with `float` or `int` as needed.

#### `update`
```rea
void update() {
  int i = 1;
  while (i <= NumBalls) {
    if (my.balls[i].active) my.balls[i].move(my.maxX, my.maxY);
    i = i + 1;
  }
  my.handleCollisions();
}
```
Moves every active ball and then processes collisions.

**Syntax notes:** The method calls another method on the same class using `my.handleCollisions()` and uses an `if` statement to guard each ball's `move` call.

#### `draw`
```rea
void draw() {
  cleardevice();
  int i = 1;
  while (i <= NumBalls) {
    if (my.balls[i].active) my.balls[i].draw();
    i = i + 1;
  }
  updatescreen();
}
```
Clears the screen, draws each ball, and presents the new frame.

**Syntax notes:** A simple loop iterates over the array, and `updatescreen()` flushes the drawing buffer.

#### `run`
```rea
void run() {
  my.init();
  writeln("Multi Bouncing Balls... Press Q to quit.");
  while (!my.quit) {
    if (keypressed()) {
      char c = readkey();
      if (toupper(c) == 'Q') my.quit = true;
    }
    my.update();
    my.draw();
    graphloop(my.FrameDelay);
  }
  closegraph();
  writeln("Demo finished.");
}
```
Initializes the application, enters the main loop, checks for user input, updates
and draws the scene each frame, and exits when the `Q` key is pressed.

**Syntax notes:** The `while (!my.quit)` loop uses logical negation. Character literals like `'Q'` are enclosed in single quotes, and the method ends with no explicit `return` because its return type is `void`.

### Program entry point
```rea
BallsApp app = new BallsApp();
app.run();
```
Creates the `BallsApp` instance and invokes its `run` method.

**Syntax notes:** Top-level code allocates a class with `new` and invokes methods using `object.method()` syntax.

## Next steps

Experiment by changing constants like `NumBalls` or adding new behavior in the
update loop. The same workflow can be used for other Rea programs.
## Tracing and inspecting execution

When diagnosing control flow, two tools are handy: VM instruction tracing and
bytecode disassembly.

- Enable a short instruction trace (first N instructions):

  ```sh
  build/bin/rea --vm-trace-head=64 Examples/rea/sdl_multibouncingballs
  ```

  The VM prints lines like:

  ```
  [VM-TRACE] IP=0000 OPC=22 STACK=0
  [VM-TRACE] IP=0032 OPC=25 STACK=1
  ```

  Where:
  - `IP` is the current instruction pointer (byte offset within the chunk)
  - `OPC` is the numeric opcode (use disassembly to see mnemonics)
  - `STACK` is the current stack depth

  Tip: you can also drop a `trace on` comment anywhere in your source to enable
  a short default trace (equivalent to `--vm-trace-head=16`).

- Disassemble the compiled bytecode to human‑readable mnemonics:

  ```sh
  build/bin/rea --no-cache --dump-bytecode Examples/rea/sdl_multibouncingballs
  ```

  This prints the bytecode and any procedures discovered. It is useful to
  confirm that top‑level calls (e.g., to a `main` routine) are being emitted.

- Bypass cached bytecode to ensure you execute the latest compile:

  ```sh
  build/bin/rea --no-cache Examples/rea/sdl_multibouncingballs
  ```

  The Rea front end caches compiled chunks under `~/.pscal/bc_cache`. Use `--no-cache`
  during iteration or delete the cache directory to force a fresh compile.
