/*
  Copyright (C) 2018-2019 SKALE Labs

  This file is part of libBLS.

  libBLS is free software: you can redistribute it and/or modify
  it under the terms of the GNU Affero General Public License as published
  by the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  libBLS is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU Affero General Public License for more details.

  You should have received a copy of the GNU Affero General Public License
  along with libBLS.  If not, see <https://www.gnu.org/licenses/>.

  @file sign_bls.cpp
  @author Oleh Nikolaiev
  @date 2019
*/

#include <bls/bls.h>
#include <tools/utils.h>

#include <fstream>

#include <third_party/json.hpp>

#include <boost/program_options.hpp>

#define EXPAND_AS_STR( x ) __EXPAND_AS_STR__( x )
#define __EXPAND_AS_STR__( x ) #x

static bool g_b_verbose_mode = false;

static bool g_b_rehash = false;

int char2int( char _input ) {
    if ( _input >= '0' && _input <= '9' )
        return _input - '0';
    if ( _input >= 'A' && _input <= 'F' )
        return _input - 'A' + 10;
    if ( _input >= 'a' && _input <= 'f' )
        return _input - 'a' + 10;
    return -1;
}

bool hex2carray( const char* _hex, uint64_t* _bin_len, uint8_t* _bin ) {
    int len = strnlen( _hex, 2 * 1024 );

    if ( len == 0 && len % 2 == 1 )
        return false;
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

void Sign( const size_t t, const size_t n, std::istream& data_file, std::ostream& outfile,
    const std::string& key, bool sign_all = true, int idx = -1 ) {
    libBLS::Bls bls_instance = libBLS::Bls( t, n );

    std::vector< uint8_t > message_data;
    uint8_t n_byte;
    while ( data_file >> n_byte ) {
        message_data.push_back( n_byte );
    }

    std::string message( message_data.cbegin(), message_data.cend() );
    auto hash_bytes_arr = std::make_shared< std::array< uint8_t, 32 > >();
    if ( g_b_rehash ) {
        std::string hash_str = cryptlite::sha256::hash_hex( message );
        for ( size_t i = 0; i < 32; i++ ) {
            hash_bytes_arr->at( i ) = static_cast< uint8_t >( hash_str[i] );
        }
    } else {
        uint64_t bin_len;
        if ( !hex2carray( message.c_str(), &bin_len, hash_bytes_arr->data() ) ) {
            throw std::runtime_error( "Invalid hash" );
        }
    }

    libff::alt_bn128_G1 hash = libBLS::ThresholdUtils::HashtoG1( hash_bytes_arr );

    nlohmann::json hash_json;
    hash_json["message"] = message;

    libff::alt_bn128_G1 common_signature;

    if ( sign_all ) {
        std::vector< libff::alt_bn128_Fr > secret_key( n );

        for ( size_t i = 0; i < n; ++i ) {
            nlohmann::json secret_key_file;

            std::ifstream infile( key + std::to_string( i ) + ".json" );
            infile >> secret_key_file;

            secret_key[i] = libff::alt_bn128_Fr(
                secret_key_file["insecureBLSPrivateKey"].get< std::string >().c_str() );
        }

        std::vector< libff::alt_bn128_G1 > signature_shares( n );
        for ( size_t i = 0; i < n; ++i ) {
            signature_shares[i] = bls_instance.Signing( hash, secret_key[i] );
        }

        std::vector< size_t > idx( t );
        for ( size_t i = 0; i < t; ++i ) {
            idx[i] = i + 1;
        }

        std::vector< libff::alt_bn128_Fr > lagrange_coeffs =
            libBLS::ThresholdUtils::LagrangeCoeffs( idx, t );

        common_signature = bls_instance.SignatureRecover( signature_shares, lagrange_coeffs );
    } else {
        libff::alt_bn128_Fr secret_key;

        nlohmann::json secret_key_file;

        std::ifstream infile( key + std::to_string( idx ) + ".json" );
        infile >> secret_key_file;

        secret_key = libff::alt_bn128_Fr(
            secret_key_file["insecureBLSPrivateKey"].get< std::string >().c_str() );

        common_signature = bls_instance.Signing( hash, secret_key );
    }

    common_signature.to_affine_coordinates();

    nlohmann::json signature;
    if ( idx >= 0 ) {
        signature["index"] = std::to_string( idx );
    }

    signature["signature"]["X"] =
        libBLS::ThresholdUtils::fieldElementToString( common_signature.X );
    signature["signature"]["Y"] =
        libBLS::ThresholdUtils::fieldElementToString( common_signature.Y );

    std::ofstream outfile_h( "hash.json" );
    outfile_h << hash_json.dump( 4 ) << "\n";

    outfile << signature.dump( 4 ) << "\n";
}

int main( int argc, const char* argv[] ) {
    std::istream* p_in = &std::cin;
    std::ostream* p_out = &std::cout;
    int r = 1;
    try {
        boost::program_options::options_description desc( "Options" );
        desc.add_options()( "help", "Show this help screen" )( "version", "Show version number" )(
            "t", boost::program_options::value< size_t >(), "Threshold" )(
            "n", boost::program_options::value< size_t >(), "Number of participants" )( "input",
            boost::program_options::value< std::string >(),
            "Input file path with containing message to sign; if not specified then use standard "
            "input" )( "j", boost::program_options::value< int >(),
            "Index of participant to sign; if not specified then all participants" )( "key",
            boost::program_options::value< std::string >(),
            "Directory with secret keys which are BLS_keys<j>.json " )( "output",
            boost::program_options::value< std::string >(),
            "Output file path to save signature to; if not specified for common signature then use "
            "standard output;" )( "v", "Verbose mode (optional)" )(
            "rehash", "if not specified, then do not hash input message" );

        boost::program_options::variables_map vm;
        boost::program_options::store(
            boost::program_options::parse_command_line( argc, argv, desc ), vm );
        boost::program_options::notify( vm );

        if ( vm.count( "help" ) || argc <= 1 ) {
            std::cout << "BLS sign tool, version " << EXPAND_AS_STR( BLS_VERSION ) << '\n'
                      << "Usage:\n"
                      << "   " << argv[0]
                      << " --t <threshold> --n <num_participants> [--j <participant>] [--input "
                         "<path>] [--output <path>] [--key <path>] [--v]"
                      << '\n'
                      << desc << '\n';
            return 0;
        }
        if ( vm.count( "version" ) ) {
            std::cout << EXPAND_AS_STR( BLS_VERSION ) << '\n';
            return 0;
        }

        if ( vm.count( "t" ) == 0 ) {
            throw std::runtime_error( "--t is missing (see --help)" );
        }

        if ( vm.count( "n" ) == 0 ) {
            throw std::runtime_error( "--n is missing (see --help)" );
        }

        if ( vm.count( "key" ) == 0 ) {
            throw std::runtime_error( "--key is missing (see --help)" );
        }

        if ( vm.count( "v" ) ) {
            g_b_verbose_mode = true;
        }

        if ( vm.count( "rehash" ) ) {
            g_b_rehash = true;
        }

        size_t t = vm["t"].as< size_t >();
        size_t n = vm["n"].as< size_t >();
        if ( g_b_verbose_mode ) {
            std::cout << "t = " << t << '\n' << "n = " << n << '\n' << '\n';
        }

        int j = -1;
        if ( vm.count( "j" ) ) {
            j = vm["j"].as< int >();
            if ( g_b_verbose_mode ) {
                std::cout << "j = " << j << '\n';
            }
        }

        std::string key = vm["key"].as< std::string >();
        if ( g_b_verbose_mode ) {
            std::cout << "key = " << key << '\n';
        }

        if ( vm.count( "input" ) ) {
            if ( g_b_verbose_mode ) {
                std::cout << "input = " << vm["input"].as< std::string >() << '\n';
            }
            p_in =
                new std::ifstream( vm["input"].as< std::string >().c_str(), std::ifstream::binary );
        }

        if ( vm.count( "output" ) ) {
            if ( g_b_verbose_mode ) {
                std::cout << "output = " << vm["output"].as< std::string >() << '\n';
            }
            p_out = new std::ofstream(
                vm["output"].as< std::string >().c_str(), std::ofstream::binary );
        }

        if ( j < 0 ) {
            Sign( t, n, *p_in, *p_out, key );
        } else {
            Sign( t, n, *p_in, *p_out, key, false, j );
        }
        r = 0;  // success
    } catch ( std::exception& ex ) {
        r = 1;
        std::string str_what = ex.what();
        if ( str_what.empty() )
            str_what = "exception without description";
        std::cerr << "exception: " << str_what << "\n";
    } catch ( ... ) {
        r = 2;
        std::cerr << "unknown exception\n";
    }
    if ( p_in != &std::cin )
        delete ( std::ifstream* ) p_in;
    if ( p_out != &std::cout )
        delete ( std::ofstream* ) p_out;
    return r;
}
