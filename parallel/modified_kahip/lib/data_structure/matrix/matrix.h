/******************************************************************************
 * matrix.h
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#ifndef MATRIX_BHHZ9T7P
#define MATRIX_BHHZ9T7P
namespace kahip::modified {
class matrix {
public:
  matrix(unsigned int dim_x, unsigned int dim_y) {};
  matrix() = default;  // Default constructor
  virtual ~matrix() = default; // Default destructor

  // Explicitly default the other special member functions
  matrix(const matrix&) = default;            // Copy constructor
  matrix& operator=(const matrix&) = default; // Copy assignment operator
  matrix(matrix&&) = default;                 // Move constructor
  matrix& operator=(matrix&&) = default;      // Move assignment operator

  virtual int  get_xy(unsigned int x, unsigned int y)            = 0;
  virtual void set_xy(unsigned int x, unsigned int y, int value) = 0;
};
}


#endif /* end of include guard: MATRIX_BHHZ9T7P */
