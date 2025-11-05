/******************************************************************************
 * normal_matrix.h
 * *
 * Source of KaHIP -- Karlsruhe High Quality Partitioning.
 * Christian Schulz <christian.schulz.phone@gmail.com>
 *****************************************************************************/

#ifndef NORMAL_MATRIX_DAUJ4JMM
#define NORMAL_MATRIX_DAUJ4JMM

#include "matrix.h"
namespace kahip::modified {
class normal_matrix : public matrix {
public:
        normal_matrix(unsigned int dim_x, unsigned int dim_y, int lazy_init_val = 0) : m_dim_x (dim_x), 
                                                                                       m_dim_y (dim_y), 
                                                                                       m_lazy_init_val ( lazy_init_val ) {
                m_internal_matrix.resize(m_dim_x); //allocate the rest lazy
        };

        int get_xy(unsigned int x, unsigned int y) override {
                if( m_internal_matrix[x].empty() ) {
                        return m_lazy_init_val;
                }
                return m_internal_matrix[x][y];
        };

        void set_xy(unsigned int x, unsigned int y, int value) override {
                //resize the fields lazy
                if( m_internal_matrix[x].empty() ) {
                        m_internal_matrix[x].resize(m_dim_y);
                        for( unsigned y_1 = 0; y_1 < m_dim_y; y_1++) {
                                m_internal_matrix[x][y_1] = m_lazy_init_val;
                        }
                }
                m_internal_matrix[x][y] = value;
        };

private:
        std::vector< std::vector<int> > m_internal_matrix;
        unsigned int m_dim_x, m_dim_y;
        int m_lazy_init_val;
};
}

#endif /* end of include guard: NORMAL_MATRIX_DAUJ4JMM */
