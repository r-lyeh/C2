#include <stdio.h>

int grid[3][4] = {{1, 2, 3, 4}, {5, 6, 7, 8}, {9, 10, 11, 12}};
char names[][8] = {"ann", "bob", "carol"};

int sum_rows(int m[][4], int rows) {
    int s = 0;
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < 4; c++) s += m[r][c];
    return s;
}

int trace(int (*m)[4]) { return m[0][0] + m[1][1] + m[2][2]; }

int main(void) {
    int id[3][3] = {{1}, {0, 1}, {0, 0, 1}};
    int flat[2][3] = {1, 2, 3, 4, 5, 6};          /* brace elision */
    double cube[2][2][2];
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++)
            for (int k = 0; k < 2; k++) cube[i][j][k] = i * 4 + j * 2 + k;
    int (*row)[4] = grid + 1;
    int *p = &grid[2][1];
    printf("sum %d trace %d id %d%d%d %d flat %d %d\n", sum_rows(grid, 3), trace(grid), id[0][0], id[1][1], id[2][2],
           id[0][2], flat[1][0], flat[0][2]);
    printf("cube %.0f %.0f sizes %d %d %d %d\n", cube[1][0][1], cube[1][1][1], (int)sizeof grid, (int)sizeof grid[0],
           (int)sizeof cube, (int)sizeof names);
    printf("row %d %d p %d names %s %s %c\n", (*row)[0], row[1][3], p[1], names[1], names[2], names[2][4]);
    return grid[2][3] + id[2][2];
}
