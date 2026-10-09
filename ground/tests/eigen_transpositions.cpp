#include <Eigen/Core>

int main()
{
    Eigen::Matrix3i matrix;
    matrix << 1, 2, 3,
              4, 5, 6,
              7, 8, 9;

    Eigen::Transpositions<3> swaps;
    swaps.setIdentity();
    swaps[0] = 1;
    swaps[1] = 2;

    Eigen::Matrix3i expectedColumns;
    expectedColumns << 3, 1, 2,
                       6, 4, 5,
                       9, 7, 8;
    Eigen::Matrix3i expectedRows;
    expectedRows << 7, 8, 9,
                    1, 2, 3,
                    4, 5, 6;

    const Eigen::Matrix3i columns = matrix * swaps.transpose();
    const Eigen::Matrix3i rows = swaps.transpose() * matrix;
    if (columns != expectedColumns || rows != expectedRows)
        return 1;

    const Eigen::Matrix3i restoredColumns = columns * swaps;
    const Eigen::Matrix3i restoredRows = swaps * rows;
    return restoredColumns == matrix && restoredRows == matrix ? 0 : 2;
}
