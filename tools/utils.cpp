/*
  Copyright (C) 2021- SKALE Labs

  This file is part of libBLS.

  libBLS is free software: you can redistribute it and/or modify
  it under the terms of the GNU Affero General Public License as published
  by the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  libBLS is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU Affero General Public License for more details.

  You should have received a copy of the GNU Affero General Public License
  along with libBLS. If not, see <https://www.gnu.org/licenses/>.

  @file utils.cpp
  @author Oleh Nikolaiev
  @date 2021
*/

#include <mutex>

#include <openssl/aes.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <tools/utils.h>


namespace libBLS {


std::atomic< bool > ThresholdUtils::is_initialized = false;

std::mutex initMutex;

void ThresholdUtils::initCurve() {
    std::lock_guard< std::mutex > lock( initMutex );
    if ( !is_initialized ) {
        libff::init_alt_bn128_params();
        is_initialized = true;
    }
}

void ThresholdUtils::checkSigners( size_t _requiredSigners, size_t _totalSigners ) {
    if ( _requiredSigners > _totalSigners ) {
        throw IsNotWellFormed( "_requiredSigners > _totalSigners" );
    }

    if ( _totalSigners == 0 ) {
        throw IncorrectInput( "_totalSigners == 0" );
    }

    if ( _requiredSigners == 0 ) {
        throw IncorrectInput( "_requiredSigners == 0" );
    }
}

std::vector< std::string > ThresholdUtils::G2ToString( libff::alt_bn128_G2 elem, int base ) {
    std::vector< std::string > pkey_str_vect;

    elem.to_affine_coordinates();

    pkey_str_vect.push_back( fieldElementToString( elem.X.c0, base ) );
    pkey_str_vect.push_back( fieldElementToString( elem.X.c1, base ) );
    pkey_str_vect.push_back( fieldElementToString( elem.Y.c0, base ) );
    pkey_str_vect.push_back( fieldElementToString( elem.Y.c1, base ) );

    return pkey_str_vect;
}

std::string ThresholdUtils::convertHexToDec( const std::string& hex_str ) {
    mpz_t dec;
    mpz_init( dec );

    std::string output;

    try {
        if ( mpz_set_str( dec, hex_str.c_str(), 16 ) == -1 ) {
            mpz_clear( dec );
            throw IsNotWellFormed( "Bad formatted hex string provided" );
        }

        char arr[mpz_sizeinbase( dec, 10 ) + 2];
        char* tmp = mpz_get_str( arr, 10, dec );
        mpz_clear( dec );

        output = tmp;
    } catch ( std::exception& e ) {
        mpz_clear( dec );
        throw IsNotWellFormed( e.what() );
    } catch ( ... ) {
        mpz_clear( dec );
        throw IsNotWellFormed( "Exception in convert hex to dec" );
    }

    return output;
}

libff::alt_bn128_G2 ThresholdUtils::stringToG2( const std::string& str ) {
    if ( str.size() != 256 ) {
        throw IncorrectInput( "Wrong string size to convert to G2" );
    }

    libff::alt_bn128_G2 ret;

    ret.Z = libff::alt_bn128_Fq2::one();

    ret.X.c0 =
        libff::alt_bn128_Fq( ThresholdUtils::convertHexToDec( str.substr( 0, 64 ) ).c_str() );
    ret.X.c1 =
        libff::alt_bn128_Fq( ThresholdUtils::convertHexToDec( str.substr( 64, 64 ) ).c_str() );
    ret.Y.c0 =
        libff::alt_bn128_Fq( ThresholdUtils::convertHexToDec( str.substr( 128, 64 ) ).c_str() );
    ret.Y.c1 = libff::alt_bn128_Fq(
        ThresholdUtils::convertHexToDec( str.substr( 192, std::string::npos ) ).c_str() );

    return ret;
}

libff::alt_bn128_G1 ThresholdUtils::stringToG1( const std::string& str ) {
    if ( str.size() != 128 ) {
        throw IncorrectInput( "Wrong string size to convert to G1" );
    }

    libff::alt_bn128_G1 ret;

    ret.Z = libff::alt_bn128_Fq::one();
    ret.X = libff::alt_bn128_Fq( ThresholdUtils::convertHexToDec( str.substr( 0, 64 ) ).c_str() );
    ret.Y = libff::alt_bn128_Fq( ThresholdUtils::convertHexToDec( str.substr( 64, 64 ) ).c_str() );

    return ret;
}

void ThresholdUtils::LagrangeCoeffs(
    const std::vector< size_t >& idx, size_t t, std::vector< libff::alt_bn128_Fr >& res) {
    if ( idx.size() < t ) {
        throw IncorrectInput( "not enough participants in the threshold group" );
    }

    libff::alt_bn128_Fr w = libff::alt_bn128_Fr::one();

    libff::alt_bn128_Fr fr_arr[t];
    for ( size_t i = 0; i < t; ++i ) {
        fr_arr[i] = libff::alt_bn128_Fr(idx[i]);
        w *= fr_arr[i];
    }

    for ( size_t i = 0; i < t; ++i ) {
        libff::alt_bn128_Fr v = fr_arr[i];

        for ( size_t j = 0; j < t; ++j ) {
            if ( j != i ) {
                if ( idx[i] == idx[j] ) {
                    throw IncorrectInput(
                        "during the interpolation, have same indexes in list of indexes" );
                }

                v *= (fr_arr[j] - fr_arr[i]);  // calculating Lagrange coefficients
            }
        }

        res[i] = w * v.invert();
    }
}

std::vector< libff::alt_bn128_Fr > ThresholdUtils::LagrangeCoeffs(
    const std::vector< size_t >& idx, size_t t) {
    std::vector< libff::alt_bn128_Fr > res(t);
    LagrangeCoeffs(idx, t, res);
    return res;
}

libff::alt_bn128_Fq ThresholdUtils::HashToFq(
    std::shared_ptr< std::array< uint8_t, 32 > > hash_byte_arr ) {
    libff::bigint< libff::alt_bn128_q_limbs > from_hex;

    std::vector< uint8_t > hex( 64 );
    for ( size_t i = 0; i < 32; ++i ) {
        hex[2 * i] = static_cast< int >( hash_byte_arr->at( i ) ) / 16;
        hex[2 * i + 1] = static_cast< int >( hash_byte_arr->at( i ) ) % 16;
    }
    mpn_set_str( from_hex.data, hex.data(), 64, 16 );

    libff::alt_bn128_Fq ret_val( from_hex );

    return ret_val;
}

libff::alt_bn128_G1 ThresholdUtils::HashtoG1(
    std::shared_ptr< std::array< uint8_t, 32 > > hash_byte_arr ) {
    libff::alt_bn128_Fq x1( HashToFq( hash_byte_arr ) );

    libff::alt_bn128_G1 result;

    while ( true ) {
        libff::alt_bn128_Fq y1_sqr = x1 ^ 3;
        y1_sqr = y1_sqr + libff::alt_bn128_coeff_b;

        libff::alt_bn128_Fq euler = y1_sqr ^ libff::alt_bn128_Fq::euler;

        if ( euler == libff::alt_bn128_Fq::one() ||
             euler == libff::alt_bn128_Fq::zero() ) {  // if y1_sqr is a square
            result.X = x1;
            libff::alt_bn128_Fq temp_y = y1_sqr.sqrt();

            mpz_t pos_y;
            mpz_init( pos_y );

            temp_y.as_bigint().to_mpz( pos_y );

            mpz_t neg_y;
            mpz_init( neg_y );

            ( -temp_y ).as_bigint().to_mpz( neg_y );

            if ( mpz_cmp( pos_y, neg_y ) < 0 ) {
                temp_y = -temp_y;
            }

            mpz_clear( pos_y );
            mpz_clear( neg_y );

            result.Y = temp_y;
            break;
        } else {
            x1 = x1 + 1;
        }
    }
    result.Z = libff::alt_bn128_Fq::one();

    return result;
}

libff::alt_bn128_G1 ThresholdUtils::HashtoG1( const std::string& message ) {
    auto hash_bytes_arr = std::make_shared< std::array< uint8_t, 32 > >();

    uint64_t bin_len;
    if ( !ThresholdUtils::hex2carray( message.c_str(), &bin_len, hash_bytes_arr->data() ) ) {
        throw std::runtime_error( "Invalid hash" );
    }

    return ThresholdUtils::HashtoG1( hash_bytes_arr );
}

bool ThresholdUtils::isStringNumber( const std::string& str ) {
    if ( str.at( 0 ) == '0' && str.length() > 1 )
        return false;
    for ( const char& c : str ) {
        if ( !( c >= '0' && c <= '9' ) ) {
            return false;
        }
    }
    return true;
}

std::string ThresholdUtils::carray2Hex( const unsigned char* d, uint64_t len ) {
    std::string _hexArray;
    _hexArray.resize( 2 * len );

    char hexval[16] = {
        '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f' };

    for ( uint64_t j = 0; j < len; j++ ) {
        _hexArray[j * 2] = hexval[( ( d[j] >> 4 ) & 0xF )];
        _hexArray[j * 2 + 1] = hexval[( d[j] ) & 0x0F];
    }

    return _hexArray;
}

int ThresholdUtils::char2int( char _input ) {
    if ( _input >= '0' && _input <= '9' )
        return _input - '0';
    if ( _input >= 'A' && _input <= 'F' )
        return _input - 'A' + 10;
    if ( _input >= 'a' && _input <= 'f' )
        return _input - 'a' + 10;
    return -1;
}

bool ThresholdUtils::hex2carray( const char* _hex, uint64_t* _bin_len, uint8_t* _bin ) {
    int len = strnlen( _hex, 2 * 1024 );

    if ( len % 2 == 1 ) {
        return false;
    }
    *_bin_len = len / 2;
    for ( int i = 0; i < len / 2; i++ ) {
        int high = char2int( ( char ) _hex[i * 2] );
        int low = char2int( ( char ) _hex[i * 2 + 1] );
        if ( high < 0 || low < 0 ) {
            return false;
        }
        _bin[i] = ( unsigned char ) ( high * 16 + low );
    }
    return true;
}

bool ThresholdUtils::checkHex( const std::string& hex ) {
    mpz_t num;
    mpz_init( num );

    if ( mpz_set_str( num, hex.c_str(), 16 ) == -1 ) {
        mpz_clear( num );
        return false;
    }
    mpz_clear( num );

    return true;
}

std::pair< libff::alt_bn128_Fq, libff::alt_bn128_Fq > ThresholdUtils::ParseHint(
    const std::string& _hint ) {
    auto position = _hint.find( ":" );

    if ( position == std::string::npos || position > BLS_MAX_COMPONENT_LEN ||
         _hint.length() - position - 1 > BLS_MAX_COMPONENT_LEN ) {
        throw IncorrectInput( "Misformatted hint" );
    }

    libff::alt_bn128_Fq y( _hint.substr( 0, position ).c_str() );
    libff::alt_bn128_Fq shift_x( _hint.substr( position + 1 ).c_str() );

    return std::make_pair( y, shift_x );
}

std::shared_ptr< std::vector< std::string > > ThresholdUtils::SplitString(
    std::shared_ptr< std::string > str, const std::string& delim ) {
    if ( !str ) {
        throw IncorrectInput( " str pointer is null in SplitString " );
    }

    std::vector< std::string > tokens;
    size_t prev = 0, pos = 0;
    do {
        pos = str->find( delim, prev );
        if ( pos == std::string::npos )
            pos = str->length();
        std::string token = str->substr( prev, pos - prev );
        if ( !token.empty() )
            tokens.push_back( token );
        prev = pos + delim.length();
    } while ( pos < str->length() && prev < str->length() );

    return std::make_shared< std::vector< std::string > >( tokens );
}

void ThresholdUtils::initAES() {
    static int init = 0;
    if ( init == 0 ) {
        // initialize openssl ciphers
        OpenSSL_add_all_ciphers();

        // initialize random number generator (for IVs)
        //RAND_load_file( "/dev/urandom", 32 );
        ++init;
    }
}

std::vector< uint8_t > ThresholdUtils::aesEncrypt(
    const std::string& plaintext, const std::string& key ) {
    initAES();

    size_t enc_length = plaintext.length() * 3;
    std::vector< unsigned char > output;
    output.resize( enc_length, '\0' );

    unsigned char iv[AES_BLOCK_SIZE];
    RAND_bytes( iv, sizeof( iv ) );
    std::copy( iv, iv + 16, output.begin() );

    int actual_size = 0, final_size = 0;
    EVP_CIPHER_CTX* e_ctx = EVP_CIPHER_CTX_new();
    EVP_EncryptInit( e_ctx, EVP_aes_256_cbc(), ( const unsigned char* ) key.c_str(), iv );
    EVP_EncryptUpdate( e_ctx, &output[64], &actual_size, ( const unsigned char* ) plaintext.data(),
        plaintext.length() );
    EVP_EncryptFinal( e_ctx, &output[64 + actual_size], &final_size );
    std::copy( iv, iv + 16, output.begin() + 16 );
    output.resize( 64 + actual_size + final_size );
    EVP_CIPHER_CTX_free( e_ctx );
    return output;
}

std::string ThresholdUtils::aesDecrypt(
    const std::vector< uint8_t >& ciphertext, const std::string& key ) {
    initAES();

    unsigned char iv[AES_BLOCK_SIZE];
    std::copy( ciphertext.begin(), ciphertext.begin() + 16, iv );
    std::vector< unsigned char > plaintext;
    plaintext.resize( ciphertext.size(), '\0' );

    int actual_size = 0, final_size = 0;
    EVP_CIPHER_CTX* d_ctx = EVP_CIPHER_CTX_new();
    EVP_DecryptInit( d_ctx, EVP_aes_256_cbc(), ( const unsigned char* ) key.c_str(), iv );
    EVP_DecryptUpdate(
        d_ctx, &plaintext[0], &actual_size, &ciphertext[64], ciphertext.size() - 64 );
    EVP_DecryptFinal( d_ctx, &plaintext[actual_size], &final_size );
    EVP_CIPHER_CTX_free( d_ctx );
    plaintext.resize( actual_size + final_size, '\0' );

    return std::string( plaintext.begin(), plaintext.end() );
}

}  // namespace libBLS
