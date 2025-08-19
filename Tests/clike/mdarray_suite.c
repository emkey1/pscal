float f_abs(float x) {
    if (x < 0.0) {
        return -x;
    }
    return x;
}

int main() {
    float TOLERANCE;
    int matrix_a[4][3];
    float cube_a[2][3][5];
    int i;
    int j;
    int k;
    int checksum_i;
    float checksum_r;

    TOLERANCE = 0.0001;
    printf("Running pscal Multi-Dimensional Array Torture Test");

    printf("");
    printf("--- Testing 2D Array (Matrix) ---");
    printf("Assigning values to matrix_a[1..3, 0..2]...");
    checksum_i = 0;
    for (i = 1; i <= 3; i = i + 1) {
        for (j = 0; j <= 2; j = j + 1) {
            matrix_a[i][j] = i * 10 + j;
            checksum_i = checksum_i + matrix_a[i][j];
        }
    }
    printf("Assignment complete.");
    if (checksum_i == 189) {
        printf("START: 2D Array Checksum after assignment: PASS");
    } else {
        printf("START: 2D Array Checksum after assignment: FAIL");
    }

    printf("Verifying individual elements...");
    if (matrix_a[1][0] == 10) {
        printf("START: 2D Access matrix_a[1, 0]: PASS");
    } else {
        printf("START: 2D Access matrix_a[1, 0]: FAIL");
    }
    if (matrix_a[1][2] == 12) {
        printf("START: 2D Access matrix_a[1, 2] (Edge): PASS");
    } else {
        printf("START: 2D Access matrix_a[1, 2] (Edge): FAIL");
    }
    if (matrix_a[2][1] == 21) {
        printf("START: 2D Access matrix_a[2, 1]: PASS");
    } else {
        printf("START: 2D Access matrix_a[2, 1]: FAIL");
    }
    if (matrix_a[3][0] == 30) {
        printf("START: 2D Access matrix_a[3, 0] (Edge): PASS");
    } else {
        printf("START: 2D Access matrix_a[3, 0] (Edge): FAIL");
    }
    if (matrix_a[3][2] == 32) {
        printf("START: 2D Access matrix_a[3, 2] (Corner): PASS");
    } else {
        printf("START: 2D Access matrix_a[3, 2] (Corner): FAIL");
    }
    matrix_a[2][1] = 999;
    if (matrix_a[2][1] == 999) {
        printf("START: 2D Modify/Access matrix_a[2, 1]: PASS");
    } else {
        printf("START: 2D Modify/Access matrix_a[2, 1]: FAIL");
    }

    printf("");
    printf("--- Testing 3D Array (Cube) ---");
    printf("Assigning values to cube_a[-1..0, 1..2, 3..4]...");
    checksum_r = 0.0;
    for (i = -1; i <= 0; i = i + 1) {
        for (j = 1; j <= 2; j = j + 1) {
            for (k = 3; k <= 4; k = k + 1) {
                cube_a[i + 1][j][k] = i * 100.0 + j * 10.0 + k;
                checksum_r = checksum_r + cube_a[i + 1][j][k];
            }
        }
    }
    printf("Assignment complete.");
    if (f_abs(checksum_r - (-252.0)) < TOLERANCE) {
        printf("START: 3D Array Checksum after assignment: PASS");
    } else {
        printf("START: 3D Array Checksum after assignment: FAIL");
    }

    printf("Verifying individual elements...");
    if (f_abs(cube_a[0][1][3] - (-87.0)) < TOLERANCE) {
        printf("START: 3D Access cube_a[-1, 1, 3] (Corner): PASS");
    } else {
        printf("START: 3D Access cube_a[-1, 1, 3] (Corner): FAIL");
    }
    if (f_abs(cube_a[0][2][4] - (-76.0)) < TOLERANCE) {
        printf("START: 3D Access cube_a[-1, 2, 4] (Edge): PASS");
    } else {
        printf("START: 3D Access cube_a[-1, 2, 4] (Edge): FAIL");
    }
    if (f_abs(cube_a[1][1][3] - 13.0) < TOLERANCE) {
        printf("START: 3D Access cube_a[0, 1, 3] (Edge): PASS");
    } else {
        printf("START: 3D Access cube_a[0, 1, 3] (Edge): FAIL");
    }
    if (f_abs(cube_a[1][2][4] - 24.0) < TOLERANCE) {
        printf("START: 3D Access cube_a[0, 2, 4] (Corner): PASS");
    } else {
        printf("START: 3D Access cube_a[0, 2, 4] (Corner): FAIL");
    }
    cube_a[1][1][3] = 9.87;
    if (f_abs(cube_a[1][1][3] - 9.87) < TOLERANCE) {
        printf("START: 3D Modify/Access cube_a[0, 1, 3]: PASS");
    } else {
        printf("START: 3D Modify/Access cube_a[0, 1, 3]: FAIL");
    }

    printf("");
    printf("Multi-Dimensional Array Torture Test Completed.");
    return 0;
}
