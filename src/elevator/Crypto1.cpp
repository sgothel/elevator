/*
 * Author: Sven Gothel <sgothel@jausoft.com>
 * Copyright (c) 2020 ZAFENA AB
 */

#include <fstream>
#include <iostream>

#include <elevator/elevator.hpp>

#include <jau/debug.hpp>

#include <botan_all.h>

using namespace elevator;

bool Cipherpack::encryptThenSign_RSA1(const std::string &enc_pub_key_fname,
                                  const std::string &sign_sec_key_fname, const std::string &passphrase,
                                  const std::string &data_fname,
                                  const std::string &outfilename, const bool overwrite) {
    Elevator::env_init();

    const uint64_t _t0 = jau::getCurrentMilliseconds();

    if( IOUtil::file_exists(outfilename) ) {
        if( overwrite ) {
            if( !IOUtil::remove(outfilename) ) {
                ERR_PRINT("Encrypt failed: Failed deletion of existing output file %s", outfilename.c_str());
                return false;
            }
        } else {
            ERR_PRINT("Encrypt failed: Not overwriting existing output file %s", outfilename.c_str());
            return false;
        }
    }
    std::ofstream outfile(outfilename, std::ios::out | std::ios::binary);
    if ( !outfile.good() || !outfile.is_open() ) {
        ERR_PRINT("Encrypt failed: Output file not open %s", outfilename.c_str());
        return false;
    }
    uint32_t header1_size;
    uint64_t out_bytes_header;

    try {
        Botan::RandomNumberGenerator& rng = Botan::system_rng();

        std::unique_ptr<Botan::Public_Key> enc_pub_key = load_public_key(enc_pub_key_fname);
        if( !enc_pub_key ) {
            return false;
        }
        std::unique_ptr<Botan::Private_Key> sign_sec_key = load_private_key(sign_sec_key_fname, passphrase);
        if( !sign_sec_key ) {
            return false;
        }

        std::unique_ptr<Botan::AEAD_Mode> aead = Botan::AEAD_Mode::create(aead_cipher_algo, Botan::ENCRYPTION);
        if(!aead) {
           ERR_PRINT("Encrypt failed: AEAD algo %s not available", aead_cipher_algo.c_str());
           return false;
        }
        const Botan::OID cipher_algo_oid = Botan::OID::from_string(aead_cipher_algo);
        if( cipher_algo_oid.empty() ) {
            ERR_PRINT("Encrypt failed: No OID defined for cypher algo %s", aead_cipher_algo.c_str());
            return false;
        }

        const Botan::AlgorithmIdentifier hash_id(rsa_hash_algo, Botan::AlgorithmIdentifier::USE_EMPTY_PARAM);
        const Botan::AlgorithmIdentifier pk_alg_id("RSA/"+rsa_padding_algo, hash_id.BER_encode());

        Botan::PK_Encryptor_EME enc(*enc_pub_key, rng, rsa_padding_algo+"(" + rsa_hash_algo + ")");

        const Botan::secure_vector<uint8_t> file_key = rng.random_vec(aead->key_spec().maximum_keylength());

        const std::vector<uint8_t> encrypted_key = enc.encrypt(file_key, rng);

        const Botan::secure_vector<uint8_t> nonce = rng.random_vec(ChaCha_Nonce_Size);
        aead->set_key(file_key);
        aead->set_associated_data_vec(encrypted_key);
        aead->start(nonce);

        {
            uint64_t payload_version = 1;
            uint64_t payload_version_parent = 0;

            Botan::secure_vector<uint8_t> header_buffer;
            header_buffer.reserve(buffer_size);

            std::vector<uint8_t> header1_size_buffer(4); // 32-bit little-endian fixed-byte-size

            // DER-Header-1 pass-1 to determine the header1_size (wired DER encoded data, uint32_t little-endian)
            {
                Botan::DER_Encoder der(header_buffer);
                der.start_sequence()
                   .encode(std::vector<uint8_t>(package_magic.begin(), package_magic.end()), Botan::ASN1_Type::OctetString)
                   .encode(header1_size_buffer, Botan::ASN1_Type::OctetString)
                   .encode(std::vector<uint8_t>(data_fname.begin(), data_fname.end()), Botan::ASN1_Type::OctetString)
                   .encode(payload_version, Botan::ASN1_Type::Integer)
                   .encode(payload_version_parent, Botan::ASN1_Type::Integer)
                   .encode(std::vector<uint8_t>(rsa_sign_algo.begin(), rsa_sign_algo.end()), Botan::ASN1_Type::OctetString)
                   .encode(pk_alg_id)
                   .encode(cipher_algo_oid)
                   .encode(encrypted_key, Botan::ASN1_Type::OctetString)
                   .encode(nonce, Botan::ASN1_Type::OctetString)
                   .end_cons(); // final data push

                header1_size = header_buffer.size();
                jau::put_uint32(header1_size_buffer.data(), 0, header1_size, true /* littleEndian */);
                DBG_PRINT("Encrypt: DER Header1 Size %" PRIu32 " bytes", header1_size);
            }
            // DER-Header-1 pass-2, producing final header1 with correct size and write
            header_buffer.clear();
            {
                Botan::DER_Encoder der(header_buffer);
                der.start_sequence()
                   .encode(std::vector<uint8_t>(package_magic.begin(), package_magic.end()), Botan::ASN1_Type::OctetString)
                   .encode(header1_size_buffer, Botan::ASN1_Type::OctetString)
                   .encode(std::vector<uint8_t>(data_fname.begin(), data_fname.end()), Botan::ASN1_Type::OctetString)
                   .encode(payload_version, Botan::ASN1_Type::Integer)
                   .encode(payload_version_parent, Botan::ASN1_Type::Integer)
                   .encode(std::vector<uint8_t>(rsa_sign_algo.begin(), rsa_sign_algo.end()), Botan::ASN1_Type::OctetString)
                   .encode(pk_alg_id)
                   .encode(cipher_algo_oid)
                   .encode(encrypted_key, Botan::ASN1_Type::OctetString)
                   .encode(nonce, Botan::ASN1_Type::OctetString)
                   .end_cons(); // data push
            }
            outfile.write((char*)header_buffer.data(), header_buffer.size());
            out_bytes_header = header_buffer.size();
            DBG_PRINT("Encrypt: DER Header1 written + %" PRIu64 " bytes -> %" PRIu64 " bytes", header_buffer.size(), out_bytes_header);

            // DER-Header-2 (signature)
            Botan::PK_Signer signer(*sign_sec_key, rng, rsa_sign_algo);
            std::vector<uint8_t> signature = signer.sign_message(header_buffer, rng);
            DBG_PRINT("Encrypt: Signature for %" PRIu64 " bytes: %s",
                    header_buffer.size(),
                    jau::bytesHexString(signature.data(), 0, signature.size(), true /* lsbFirst */).c_str());
            header_buffer.clear();
            {
                Botan::DER_Encoder der(header_buffer);
                der.start_sequence()
                   .encode(signature, Botan::ASN1_Type::OctetString)
                   .end_cons();
            }
            outfile.write((char*)header_buffer.data(), header_buffer.size());
            out_bytes_header += header_buffer.size();
            DBG_PRINT("Encrypt: DER Header2 written + %" PRIu64 " bytes -> %" PRIu64 " bytes", header_buffer.size(), out_bytes_header);
        }

        uint64_t out_bytes_total = outfile.tellp();
        if( out_bytes_header != out_bytes_total ) {
            ERR_PRINT("Encrypt: DER Header done, %" PRIu64 " header != %" PRIu64 " total bytes", out_bytes_header, out_bytes_total);
        } else {
            DBG_PRINT("Encrypt: DER Header done, %" PRIu64 " header == %" PRIu64 " total bytes", out_bytes_header, out_bytes_total);
        }

        uint64_t out_bytes_payload = 0;
        auto consume_data = [&](Botan::secure_vector<uint8_t>& data, bool is_final) {
            if( !is_final ) {
                aead->update(data);
                outfile.write(reinterpret_cast<char*>(data.data()), data.size());
                out_bytes_payload += data.size();
                DBG_PRINT("Encrypt: EncPayload written0 + %" PRIu64 " bytes -> %" PRIu64 " bytes", data.size(), out_bytes_payload);
            } else {
                aead->finish(data);
                outfile.write(reinterpret_cast<char*>(data.data()), data.size());
                out_bytes_payload += data.size();
                DBG_PRINT("Encrypt: EncPayload writtenF + %" PRIu64 " bytes -> %" PRIu64 " bytes", data.size(), out_bytes_payload);
            }
        };
        Botan::secure_vector<uint8_t> io_buffer;
        io_buffer.reserve(buffer_size);
        const ssize_t in_bytes_total = IOUtil::read_file(data_fname, io_buffer, consume_data);

        if ( 0>in_bytes_total || outfile.fail() ) {
            ERR_PRINT("Encrypt failed: Output file write failed %s", outfilename.c_str());
            IOUtil::remove(outfilename);
            return false;
        }

        out_bytes_total = outfile.tellp();
        if( out_bytes_header + out_bytes_payload != out_bytes_total ) {
            ERR_PRINT("Encrypt: Writing done, %s header + %s payload != %s total bytes for %s bytes input",
                    jau::to_decstring(out_bytes_header).c_str(),
                    jau::to_decstring(out_bytes_payload).c_str(),
                    jau::to_decstring(out_bytes_total).c_str(),
                    jau::to_decstring(in_bytes_total).c_str());
        } else if( jau::environment::get().verbose ) {
            jau::PLAIN_PRINT(true, "Encrypt: Writing done, %s header + %s payload == %s total bytes for %s bytes input, ratio %lf out/in",
                    jau::to_decstring(out_bytes_header).c_str(),
                    jau::to_decstring(out_bytes_payload).c_str(),
                    jau::to_decstring(out_bytes_total).c_str(),
                    jau::to_decstring(in_bytes_total).c_str(), (double)out_bytes_total/(double)in_bytes_total);
        }

        const uint64_t _td_ms = jau::getCurrentMilliseconds() - _t0; // in milliseconds
        IOUtil::print_stats("Encrypt", out_bytes_total, _td_ms);
    } catch (std::exception &e) {
        ERR_PRINT("Encrypt failed: Caught exception: %s", e.what());
        IOUtil::remove(outfilename);
        return false;
    }

    return true;
}

bool Cipherpack::checkSignThenDecrypt_RSA1(const std::string &sign_pub_key_fname,
                                       const std::string &dec_sec_key_fname, const std::string &passphrase,
                                       const std::string &data_fname,
                                       const std::string &outfilename, const bool overwrite) {
    Elevator::env_init();

    const uint64_t _t0 = jau::getCurrentMilliseconds();

    if( IOUtil::file_exists(outfilename) ) {
        if( overwrite ) {
            if( !IOUtil::remove(outfilename) ) {
                ERR_PRINT("Decrypt failed: Failed deletion of existing output file %s", outfilename.c_str());
                return false;
            }
        } else {
            ERR_PRINT("Decrypt failed: Not overwriting existing output file %s", outfilename.c_str());
            return false;
        }
    }
    std::ofstream outfile(outfilename, std::ios::out | std::ios::binary);
    if ( !outfile.good() || !outfile.is_open() ) {
        ERR_PRINT("Decrypt failed: Output file not open %s", outfilename.c_str());
        return false;
    }

    try {
        Botan::RandomNumberGenerator& rng = Botan::system_rng();

        std::unique_ptr<Botan::Public_Key> sign_pub_key = load_public_key(sign_pub_key_fname);
        if( !sign_pub_key ) {
            return false;
        }
        std::unique_ptr<Botan::Private_Key> dec_sec_key = load_private_key(dec_sec_key_fname, passphrase);
        if( !dec_sec_key ) {
            return false;
        }

        uint32_t header1_size;

        std::vector<uint8_t> package_magic_charvec;
        std::vector<uint8_t> filename_charvec;
        uint64_t payload_version;
        uint64_t payload_version_parent;
        std::vector<uint8_t> sign_algo_charvec;
        Botan::AlgorithmIdentifier pk_alg_id;
        Botan::OID cipher_algo_oid;
        std::vector<uint8_t> encrypted_key;
        std::vector<uint8_t> nonce;

        try {
            // DER-Header-1 snoop header1-size
            {
                std::vector<uint8_t> header1_size_buffer; // 32-bit little-endian fixed-byte-size

                Botan::DataSource_Stream input(data_fname, true /* use_binary */);
                Botan::BER_Decoder ber(input);
                ber.start_sequence()
                   .decode(package_magic_charvec, Botan::ASN1_Type::OctetString)
                   .decode(header1_size_buffer, Botan::ASN1_Type::OctetString);

                {
                    const std::string s(reinterpret_cast<char*>(package_magic_charvec.data()), package_magic_charvec.size());
                    if( s.empty() ) {
                       ERR_PRINT("Decrypt failed: Unknown package_magic in %s", data_fname.c_str());
                       IOUtil::remove(outfilename);
                       return false;
                    }
                    DBG_PRINT("Decrypt: package_magic is %s", s.c_str());
                    if( s != package_magic ) {
                       ERR_PRINT("Decrypt failed: Expected package magic %s, but got %s in %s",
                               package_magic.c_str(), s.c_str(), data_fname.c_str());
                       IOUtil::remove(outfilename);
                       return false;
                    }
                }
                {
                    if( 4 != header1_size_buffer.size() ) {
                        ERR_PRINT("Decrypt failed: Expected header1_size element of 4 bytes, but got % " PRIu64 " in %s",
                                header1_size_buffer.size(), data_fname.c_str());
                        IOUtil::remove(outfilename);
                        return false;
                    }
                    header1_size = jau::get_uint32(header1_size_buffer.data(), 0, true /* littleEndian */);
                    DBG_PRINT("Decrypt: DER Header1 Size %" PRIu32 " bytes", header1_size);
                }
            }
        } catch (Botan::Decoding_Error &e) {
            ERR_PRINT("Decrypt failed: Header-1a Invalid input file format: file %s, %s", data_fname, e.what());
            IOUtil::remove(outfilename);
            return false;
        }

        Botan::DataSource_Stream input(data_fname, true /* use_binary */);

        try {
            // DER-Header-1 read into memory
            Botan::secure_vector<uint8_t> header_buffer(header1_size);

            const uint64_t header1_size_read = input.read(header_buffer.data(), header1_size);
            if( header1_size_read != header1_size ) {
                ERR_PRINT("Decrypt failed: Expected header1_size % " PRIu64 ", but got % " PRIu64 " in %s",
                        header1_size, header1_size_read, data_fname.c_str());
                IOUtil::remove(outfilename);
                return false;
            }

            {
                std::vector<uint8_t> header1_size_buffer_dummy;
                Botan::BER_Decoder ber(header_buffer);
                ber.start_sequence()
                   .decode(package_magic_charvec, Botan::ASN1_Type::OctetString)
                   .decode(header1_size_buffer_dummy, Botan::ASN1_Type::OctetString)
                   .decode(filename_charvec, Botan::ASN1_Type::OctetString)
                   .decode(payload_version, Botan::ASN1_Type::Integer)
                   .decode(payload_version_parent, Botan::ASN1_Type::Integer)
                   .decode(sign_algo_charvec, Botan::ASN1_Type::OctetString)
                   .decode(pk_alg_id)
                   .decode(cipher_algo_oid)
                   .decode(encrypted_key, Botan::ASN1_Type::OctetString)
                   .decode(nonce, Botan::ASN1_Type::OctetString)
                   .end_cons();
            }
            std::vector<uint8_t> signature;
            {
                Botan::BER_Decoder ber(input);
                ber.start_sequence()
                   .decode(signature, Botan::ASN1_Type::OctetString)
                   // .end_cons() // encrypted data follows ..
                   ;
            }
            DBG_PRINT("Decrypt: Signature for %" PRIu64 " bytes: %s",
                    header_buffer.size(),
                    jau::bytesHexString(signature.data(), 0, signature.size(), true /* lsbFirst */).c_str());

            Botan::PK_Verifier verifier(*sign_pub_key, rsa_sign_algo);
            verifier.update(header_buffer);
            if( !verifier.check_signature(signature) ) {
                ERR_PRINT("Decrypt failed: Signature mismatch on %" PRIu64 " bytes, received signature %s in %s",
                        header_buffer.size(),
                        jau::bytesHexString(signature.data(), 0, signature.size(), true /* lsbFirst */).c_str(),
                        data_fname.c_str());
                IOUtil::remove(outfilename);
                return false;
            }
        } catch (Botan::Decoding_Error &e) {
            ERR_PRINT("Decrypt failed: Invalid input file format: file %s, %s", data_fname, e.what());
            IOUtil::remove(outfilename);
            return false;
        }

        {
            const std::string s(reinterpret_cast<char*>(filename_charvec.data()), filename_charvec.size());
            if( s.empty() ) {
               ERR_PRINT("Decrypt failed: Unknown filename in %s", outfilename.c_str());
               IOUtil::remove(outfilename);
               return false;
            }
            DBG_PRINT("Decrypt: filename is %s", s.c_str());
        }
        DBG_PRINT("Decrypt: payload version %s (parent %s)",
                jau::to_decstring(payload_version).c_str(),
                jau::to_decstring(payload_version_parent).c_str());

        const std::string sign_algo(reinterpret_cast<char*>(sign_algo_charvec.data()), sign_algo_charvec.size());
        {
            if( sign_algo.empty() ) {
               ERR_PRINT("Decrypt failed: Unknown signing algo in %s", outfilename.c_str());
               IOUtil::remove(outfilename);
               return false;
            }
            DBG_PRINT("Decrypt: sign algo is %s", sign_algo.c_str());
            if( sign_algo != rsa_sign_algo) {
               ERR_PRINT("Decrypt failed: Expected signing algo %s, but got %s in %s",
                       rsa_sign_algo.c_str(), sign_algo.c_str(), outfilename.c_str());
               IOUtil::remove(outfilename);
               return false;
            }
        }
        {
            const std::string padding_combo = "RSA/"+rsa_padding_algo;
            const Botan::OID pk_alg_oid = pk_alg_id.get_oid();
            const std::string pk_algo_str = Botan::OIDS::oid2str_or_empty(pk_alg_oid);
            DBG_PRINT("Decrypt: ciphertext encryption/padding algo is %s -> %s", pk_alg_oid.to_string().c_str(), pk_algo_str.c_str());
            if( pk_algo_str != padding_combo ) {
                ERR_PRINT("Decrypt failed: Expected ciphertext encryption/padding algo %s, but got %s in %s",
                        padding_combo.c_str(), pk_algo_str.c_str(), outfilename.c_str());
                IOUtil::remove(outfilename);
                return false;
            }
        }
        {
            Botan::AlgorithmIdentifier hash_algo_id;
            Botan::BER_Decoder( pk_alg_id.get_parameters() ).decode(hash_algo_id);
            const std::string hash_algo = Botan::OIDS::oid2str_or_empty(hash_algo_id.get_oid());
            if( hash_algo.empty() ) {
                ERR_PRINT("Decrypt failed: Unknown hash function used with %s padding, OID is %s in %s",
                        rsa_padding_algo.c_str(), hash_algo_id.get_oid().to_string().c_str(), outfilename.c_str());
                IOUtil::remove(outfilename);
                return false;
            }
            DBG_PRINT("Decrypt: hash function for %s padding is %s", rsa_padding_algo.c_str(), hash_algo.c_str());
            if( hash_algo != rsa_hash_algo ) {
               ERR_PRINT("Decrypt failed: Expected hash function for % padding is %s, but got %s in %s",
                       rsa_padding_algo.c_str(), rsa_hash_algo.c_str(), hash_algo.c_str(), outfilename.c_str());
               IOUtil::remove(outfilename);
               return false;
            }
            if( !hash_algo_id.get_parameters().empty() ) {
                ERR_PRINT("Decrypt failed: Unknown %s padding - %s hash function parameter used in %s",
                        rsa_padding_algo.c_str(), hash_algo.c_str(), outfilename.c_str());
                IOUtil::remove(outfilename);
                return false;
            }
        }


        const std::string cipher_algo = Botan::OIDS::oid2str_or_empty(cipher_algo_oid);
        {
            if( cipher_algo.empty() ) {
               ERR_PRINT("Decrypt failed: Unknown ciphertext encryption algo in %s", outfilename.c_str());
               IOUtil::remove(outfilename);
               return false;
            }
            DBG_PRINT("Decrypt: ciphertext encryption algo is %s", cipher_algo.c_str());
            if( cipher_algo != aead_cipher_algo) {
               ERR_PRINT("Decrypt failed: Expected ciphertext encryption algo %s, but got %s in %s",
                       aead_cipher_algo.c_str(), cipher_algo.c_str(), outfilename.c_str());
               IOUtil::remove(outfilename);
               return false;
            }
        }

        std::unique_ptr<Botan::AEAD_Mode> aead = Botan::AEAD_Mode::create_or_throw(cipher_algo, Botan::DECRYPTION);
        if(!aead) {
           ERR_PRINT("Decrypt failed: Cipher algo %s not available", cipher_algo.c_str());
           return false;
        }

        const size_t expected_keylen = aead->key_spec().maximum_keylength();

        Botan::PK_Decryptor_EME dec(*dec_sec_key, rng, rsa_padding_algo+"(" + rsa_hash_algo + ")");

        const Botan::secure_vector<uint8_t> file_key =
                dec.decrypt_or_random(encrypted_key.data(), encrypted_key.size(), expected_keylen, rng);

        aead->set_key(file_key);
        aead->set_associated_data_vec(encrypted_key);
        aead->start(nonce);

        uint64_t out_bytes_payload = 0;
        auto consume_data = [&](Botan::secure_vector<uint8_t>& data, bool is_final) {
            if( !is_final ) {
                aead->update(data);
                outfile.write(reinterpret_cast<char*>(data.data()), data.size());
                out_bytes_payload += data.size();
                DBG_PRINT("Decrypt: EncPayload written0 + %" PRIu64 " bytes -> %" PRIu64 " bytes", data.size(), out_bytes_payload);
            } else {
                // DBG_PRINT("Decrypt: p111a size %" PRIu64 ", capacity %" PRIu64 "", data.size(), data.capacity());
                // DBG_PRINT("Decrypt: p111a data %s",
                //           jau::bytesHexString(data.data(), 0, data.size(), true /* lsbFirst */).c_str());
                aead->finish(data);
                // DBG_PRINT("Decrypt: p111b size %" PRIu64 ", capacity %" PRIu64 "", data.size(), data.capacity());
                // DBG_PRINT("Decrypt: p111b data %s",
                //           jau::bytesHexString(data.data(), 0, data.size(), true /* lsbFirst */).c_str());
                outfile.write(reinterpret_cast<char*>(data.data()), data.size());
                out_bytes_payload += data.size();
                DBG_PRINT("Decrypt: EncPayload writtenF + %" PRIu64 " bytes -> %" PRIu64 " bytes", data.size(), out_bytes_payload);
            }
        };
        Botan::secure_vector<uint8_t> io_buffer;
        io_buffer.reserve(buffer_size);
        const ssize_t in_bytes_total = IOUtil::read_stream(input, io_buffer, consume_data);

        if ( 0>in_bytes_total || outfile.fail() ) {
            ERR_PRINT("Decrypt failed: Output file write failed %s", outfilename.c_str());
            IOUtil::remove(outfilename);
            return false;
        }

        const uint64_t out_bytes_total = outfile.tellp();

        if( out_bytes_payload != out_bytes_total ) {
            ERR_PRINT("Decrypt: Writing done, %s payload != %s total bytes for %s bytes input",
                    jau::to_decstring(out_bytes_payload).c_str(), jau::to_decstring(out_bytes_total).c_str(),
                    jau::to_decstring(in_bytes_total).c_str());
        } else {
            WORDY_PRINT("Decrypt: Writing done, %s total bytes from %s bytes input, ratio %lf in/out",
                    jau::to_decstring(out_bytes_total).c_str(),
                    jau::to_decstring(in_bytes_total).c_str(), (double)out_bytes_total/(double)in_bytes_total);
        }

        const uint64_t _td_ms = jau::getCurrentMilliseconds() - _t0; // in milliseconds
        IOUtil::print_stats("Decrypt", out_bytes_total, _td_ms);
    } catch (std::exception &e) {
        ERR_PRINT("Decrypt failed: Caught exception: %s", e.what());
        IOUtil::remove(outfilename);
        return false;
    }

    return true;
}

