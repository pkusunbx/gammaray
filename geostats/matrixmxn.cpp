#include "matrixmxn.h"
#include "spectral/spectral.h"
#include "spectral/svd.h"
#include "domain/application.h"
#include <Eigen/Core>
#include <Eigen/LU>

//============================= SPECIALIZATIONS FOR DOUBLE =============================================

template <>
MatrixNXM<double>::MatrixNXM( const spectral::array& array ) :
	_n( array.M() ), //yes, N is swapped with M indeed.
	_m( array.N() ),
	_values( _n*_m, 0.0 )
{
	for( uint i = 0; i < _n; ++i )
		for( uint j = 0; j < _m; ++j )
			(*this)(i,j) = array(i,j);
}

template <>
void MatrixNXM<double>::invertWithSVD(){
	MatrixNXM<double> &a = *this;

	//Make a spectral-compatible copy of this matrix.
	spectral::array A( (spectral::index)a.getN(), (spectral::index)a.getM() );
	for( int i = 0; i < a.getN(); ++i)
		for( int j = 0; j < a.getM(); ++j)
			A(i,j) = a(i,j);

	//Get the U, Sigma and V* matrices from SVD on A.
	spectral::SVD svd = spectral::svd( A );
	spectral::array U = svd.U();
	spectral::array Sigma = svd.S();
	spectral::array V = svd.V(); //SVD yields V* already transposed, that is V, but to check, you must transpose V
								 //to get A = U.Sigma.V*

	//Make a full Sigma matrix (to be compatible with multiplication with the other matrices)
	{
		spectral::array SigmaTmp( (spectral::index)a.getN(), (spectral::index)a.getM() );
		for( int i = 0; i < a.getN(); ++i)
			SigmaTmp(i, i) = Sigma.d_[i];
		Sigma = SigmaTmp;
	}

	//Make U*
	//U contains only real numbers, thus U's transpose conjugate is its transpose.
	spectral::array Ustar( U );
	//transpose U to get U*
	{
		Eigen::MatrixXd tmp = spectral::to_2d( Ustar );
		tmp.transposeInPlace();
		Ustar = spectral::to_array( tmp );
	}

	//Make Sigmadagger (pseudoinverse of Sigma)
	spectral::array SigmaDagger( Sigma );
	{
		//compute reciprocals of the non-zero elements in the main diagonal.
		for( int i = 0; i < a.getN(); ++i){
			double value = SigmaDagger(i, i);
			//only absolute values greater than the machine epsilon are considered non-zero.
			if( std::abs( value ) > std::numeric_limits<double>::epsilon() )
				SigmaDagger( i, i ) = 1.0 / value;
			else
				SigmaDagger( i, i ) = 0.0;
		}
		//transpose
		Eigen::MatrixXd tmp = spectral::to_2d( SigmaDagger );
		tmp.transposeInPlace();
		SigmaDagger = spectral::to_array( tmp );
	}

	//Make Adagger (pseudoinverse of A) by "reversing" the transform U.Sigma.V*,
	//hence, Adagger = V.Sigmadagger.Ustar .
	spectral::array Adagger;
	{
		Eigen::MatrixXd eigenV = spectral::to_2d( V );
		Eigen::MatrixXd eigenSigmadagger = spectral::to_2d( SigmaDagger );
		Eigen::MatrixXd eigenUstar = spectral::to_2d( Ustar );
		Eigen::MatrixXd eigenAdagger = eigenV * eigenSigmadagger * eigenUstar;
		Adagger = spectral::to_array( eigenAdagger );
	}

	//Return the result.
	for( int i = 0; i < a.getN(); ++i)
		for( int j = 0; j < a.getM(); ++j)
			a(i,j) = Adagger(i,j);
}

template<>
void MatrixNXM<double>::invertWithEigen()
{
	MatrixNXM<double> &a = *this;
	assert( a._m == a._n && "MatrixNXM<double> MatrixNXM<double>::invertWithEigen(): matrix is not square.  Use invertWithSVD() instead." );

	//Convert this to Eigen matrix.
	Eigen::MatrixXd A( _n, _m );
	for (size_t i = 0; i < _n; ++i)
		for (size_t j = 0; j < _m; ++j)
			A(i, j) = a(i, j);

	//Invert. Hopefully it is faster than the direct multiplication between GammaRat matrices with invertWithGaussJordan().
	const Eigen::MatrixXd& C = A.inverse();

	//Convert restult back to this matrix.
	for(uint i = 0; i < C.rows(); ++i)
		for(uint j = 0; j < C.cols(); ++j)
			a(i,j) = C(i,j);

}


template<>
void MatrixNXM<double>::invertWithGaussJordan(){
	/* This is a Gauss-Jordan method implemented from the code in Numerical Recipes, 3rd edition.
	   It is intended to solve a linear system, but it was modified just perform invertion.
	*/
	MatrixNXM<double> &a = *this;
	assert( a._m == a._n && "MatrixNXM<double> MatrixNXM<double>::invertWithGaussJordan(): matrix is not square.  Use invertWithSVD() instead." );
	// the independent terms (right-hand side) vector is created with dummy values,
	// since we're not interested in solving a system.
	int i, icol, irow, j, k, l , ll, n = a.getN();
	double big, dum, pivinv;
	std::vector<int> indxc(n), indxr(n), ipiv(n); //index bookkeeping vectors

    icol = irow = 0; //get rid of compiler warning (uninitialized variables)

	ipiv.assign(n, 0);
	for ( i=0; i<n; ++i) {
		big = 0.0;
		for (j=0; j<n; ++j)
			if (ipiv[j] != 1)
				for (k=0; k<n; ++k) {
					if (ipiv[k] == 0) {
						if ( std::abs(a(j,k)) >= big) {
							big = std::abs( a(j,k) );
							irow = j;
							icol = k;
						}
					}
				}
		++(ipiv[icol]);
		if (irow != icol) {
			for (l=0; l<n; ++l)
				std::swap( a(irow,l), a(icol,l) );
		}
		indxr[i] = irow;
		indxc[i] = icol;
		if ( a(icol, icol) == 0.0 ){
            Application::instance()->logError("MatrixNXM<double>::invertWithGaussJordan(): Singular matrix.  Operation aborted.  Resulting matrix inconsistent.");
			return;
		}
		pivinv = 1.0 / a(icol,icol);
		a(icol, icol) = 1.0;
		for (l=0; l<n; ++l) a(icol,l) *= pivinv;
		for (ll=0; ll<n; ++ll)
			if (ll != icol) {
				dum=a(ll, icol);
				a(ll, icol) = 0.0;
				for (l=0;l<n;++l)
					a(ll, l) -= a(icol, l) * dum;
			}
	}
	for (l=n-1; l>=0; --l) {
		if (indxr[l] != indxc[l])
			for ( k=0; k<n; ++k)
				std::swap( a(k,indxr[l]), a(k,indxc[l]) );
	}
}

template <>
spectral::array MatrixNXM<double>::toSpectralArray() const{
	const MatrixNXM<double> &a = *this;
	//Make a spectral-compatible copy of this matrix.
	spectral::array A( (spectral::index)a.getN(), (spectral::index)a.getM() );
	for( int i = 0; i < a.getN(); ++i)
		for( int j = 0; j < a.getM(); ++j)
			A(i,j) = a(i,j);
	return A;
}

template <>
MatrixNXM<double> MatrixNXM<double>::getTranspose( ) const {
   spectral::array a = spectral::transpose( (*this).toSpectralArray() );
   MatrixNXM<double> result( a.M_, a.N_);
   for(uint i = 0; i < a.M_; ++i)
	   for(uint j = 0; j < a.N_; ++j)
			result(i,j) = a(i,j);
   return result;
}

template <>
void MatrixNXM<double>::print() const{
    spectral::array a = toSpectralArray();
    spectral::print( a );
}

//TODO: naive matrix multiplication, improve performance (e.g. parallel) or use spectral::'s classes/methods
//template <>
//MatrixNXM<double> MatrixNXM<double>::operator*(const MatrixNXM<double>& b) {
//   MatrixNXM<double>& a = *this;
//   assert( a._m == b._n && "MatrixNXM<T> MatrixNXM<T>::operator*(): operands are matrices incompatible for multiplication." );
//   MatrixNXM<double> result( a._n, b._m );
//   for(uint i = 0; i < a._n; ++i)
//	   for(uint j = 0; j < b._m; ++j)
//		   for(uint k = 0; k < a._m; ++k) //a._m (number of cols) is supposed to be == b._n (number of rows)
//			   result(i,j) += a(i,k) * b(k,j);
//   return result;
//}

template <>
MatrixNXM<double> MatrixNXM<double>::operator*(const MatrixNXM<double>& b) {
   MatrixNXM<double>& a = *this;
   assert( a._m == b._n && "MatrixNXM<T> MatrixNXM<T>::operator*(): operands are matrices incompatible for multiplication." );

   //Convert LHS operand (this) to Eigen matrix.
   Eigen::MatrixXd A( _n, _m );
   for (size_t i = 0; i < _n; ++i)
	   for (size_t j = 0; j < _m; ++j)
		   A(i, j) = a(i, j);

   //Convert RHS operand (the other) to Eigen matrix.
   Eigen::MatrixXd B( b._n , b._m );
   for (size_t i = 0; i < b._n; ++i)
	   for (size_t j = 0; j < b._m; ++j)
		   B(i, j) = b(i, j);

   //Multiply. Hopefully it is faster than the naive direct multiplication between GammaRat matrices.
   const Eigen::MatrixXd& C = A * B;

   //Convert restult back to GammaRay matrix.
   MatrixNXM<double> result( C.rows(), C.cols() );
   for(uint i = 0; i < C.rows(); ++i)
	   for(uint j = 0; j < C.cols(); ++j)
		   result(i,j) = C(i,j);

	return result;
}

//============================= SPECIALIZATIONS FOR FLOAT (NONE YET) =============================================
