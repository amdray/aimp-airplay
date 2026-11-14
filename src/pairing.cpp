#include <windows.h>
#include "pairing.h"
#include "pairing_transport.h"
#include "pairing_plist.h"
#include "crypto_runtime.h"
#include <openssl/srp.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <algorithm>
#include <stdexcept>
#include <cstring>
namespace airplay {
namespace {
using pairing::Bytes;
void Require(bool valid, const char* error = "Некорректный ответ Apple TV") {
    if (!valid) throw std::runtime_error(error);
}
using Bn = std::unique_ptr<BIGNUM, decltype(&BN_free)>;
Bn Big(const Bytes& bytes) { auto p = BN_bin2bn(bytes.data(), int(bytes.size()), nullptr); Require(p != nullptr, "Ошибка SRP"); return Bn(p, BN_free); }
Bytes Binary(const BIGNUM* bn, int width = 0) {
    Require(bn != nullptr, "Ошибка SRP"); Bytes result(width ? width : BN_num_bytes(bn));
    Require((width ? BN_bn2binpad(bn, result.data(), width) : BN_bn2bin(bn, result.data())) == int(result.size()), "Ошибка SRP"); return result;
}
Bytes Sha1(const Bytes& bytes) { Bytes out(20); Require(SHA1(bytes.data(), bytes.size(), out.data()) != nullptr, "Ошибка SHA1"); return out; }
void Append(Bytes& to, const Bytes& from) { to.insert(to.end(), from.begin(), from.end()); }
Bytes Digest(const char* label, const Bytes& key) {
    SHA512_CTX ctx{}; Bytes out(64);
    Require(SHA512_Init(&ctx) && SHA512_Update(&ctx, label, strlen(label)) && SHA512_Update(&ctx, key.data(), key.size()) && SHA512_Final(out.data(), &ctx), "Ошибка SHA512");
    out.resize(16); return out;
}
}
struct PinPairing::Impl {
    std::shared_ptr<CryptoRuntime> runtime;
    std::wstring receiverId;
    pairing::Transport transport;
    Impl(const Receiver& receiver, std::shared_ptr<CryptoRuntime> runtime)
        : runtime(std::move(runtime)), receiverId(receiver.id), transport(receiver.host, receiver.port) {
        if (!this->runtime) throw std::invalid_argument("PIN pairing requires CryptoRuntime");
    }
};
PinPairing::PinPairing(const Receiver& receiver, std::shared_ptr<CryptoRuntime> runtime)
    : impl_(std::make_unique<Impl>(receiver, std::move(runtime))) {}
PinPairing::~PinPairing() = default;
void PinPairing::Cancel() noexcept { impl_->transport.Cancel(); }
void PinPairing::Start() { impl_->transport.Connect(); impl_->transport.Post("/pair-pin-start", {}); }
std::string PinPairing::Finish(const std::string& pin) {
    Require(pin.size() == 4 && std::all_of(pin.begin(), pin.end(), [](char c) { return c >= '0' && c <= '9'; }), "Введите четыре цифры с экрана Apple TV");
    std::wstring id = impl_->receiverId;
    if (id.rfind(L"mdns:", 0) == 0) id.erase(0, 5);
    id.resize(std::min({id.size(), id.find(L'@'), size_t(16)}));
    Require(!id.empty() && std::all_of(id.begin(), id.end(), [](wchar_t c) { return c > 0 && c < 128; }));
    std::string user(id.begin(), id.end());
    auto challenge = pairing::DecodePlist(impl_->transport.Post("/pair-setup-pin", pairing::EncodePlist({{"method", {'p','i','n'}}, {"user", Bytes(user.begin(), user.end())}}, true)));
    auto pk = challenge["pk"], salt = challenge["salt"];
    Require(!pk.empty() && pk.size() <= 256 && !salt.empty() && salt.size() <= 32);
    auto gn = SRP_get_default_gN("2048"); Require(gn != nullptr, "Ошибка SRP");
    Bytes seed(32); Require(RAND_bytes(seed.data(), int(seed.size())) == 1, "Ошибка генерации ключа");
    auto a = Big(seed), B = Big(pk), s = Big(salt);
    Require(SRP_Verify_B_mod_N(B.get(), gn->N) == 1, "Некорректный ключ Apple TV");
    Bn A(SRP_Calc_A(a.get(), gn->N, gn->g), BN_free); Require(A != nullptr, "Ошибка SRP");
    Bn x(SRP_Calc_x(s.get(), user.c_str(), pin.c_str()), BN_free), u(SRP_Calc_u(A.get(), B.get(), gn->N), BN_free);
    Require(x != nullptr && u != nullptr, "Ошибка SRP");
    Bn S(SRP_Calc_client_key(gn->N, B.get(), gn->g, x.get(), a.get(), u.get()), BN_free); Require(S != nullptr, "Ошибка SRP");
    auto padded = Binary(S.get(), 256); padded.insert(padded.end(), 4, 0);
    auto K = Sha1(padded); padded.back() = 1; Append(K, Sha1(padded));
    auto proof = Sha1(Binary(gn->N)), g = Sha1(Binary(gn->g)); for (size_t i = 0; i < 20; ++i) proof[i] ^= g[i];
    Append(proof, Sha1(Bytes(user.begin(), user.end()))); Append(proof, Binary(s.get()));
    Append(proof, Binary(A.get(), 256)); Append(proof, Binary(B.get(), 256)); Append(proof, K); proof = Sha1(proof);
    auto response = pairing::DecodePlist(impl_->transport.Post("/pair-setup-pin", pairing::EncodePlist({{"pk", Binary(A.get())}, {"proof", proof}})));
    Require(response["proof"].size() == 20, "Apple TV не подтвердила код");
    auto expected = Binary(A.get()); Append(expected, proof); Append(expected, K); expected = Sha1(expected);
    unsigned difference = 0; for (size_t i = 0; i < 20; ++i) difference |= expected[i] ^ response["proof"][i];
    Require(difference == 0, "Apple TV не подтвердила код");
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, seed.data(), seed.size()), EVP_PKEY_free);
    Require(key != nullptr, "Ошибка ключа привязки"); Bytes publicKey(32); size_t keySize = publicKey.size();
    Require(EVP_PKEY_get_raw_public_key(key.get(), publicKey.data(), &keySize) == 1 && keySize == 32, "Ошибка ключа привязки");
    auto aesKey = Digest("Pair-Setup-AES-Key", K), iv = Digest("Pair-Setup-AES-IV", K); ++iv.back();
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    Require(ctx != nullptr, "Ошибка AES"); Bytes encrypted(48), tag(16); int size = 0, tail = 0;
    Require(EVP_EncryptInit(ctx.get(), EVP_aes_128_gcm(), nullptr, nullptr) == 1 && EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, 16, nullptr) == 1 &&
        EVP_EncryptInit(ctx.get(), nullptr, aesKey.data(), iv.data()) == 1 && EVP_EncryptUpdate(ctx.get(), encrypted.data(), &size, publicKey.data(), 32) == 1 &&
        EVP_EncryptFinal(ctx.get(), encrypted.data() + size, &tail) == 1 && EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, 16, tag.data()) == 1, "Ошибка AES");
    encrypted.resize(size + tail); impl_->transport.Post("/pair-setup-pin", pairing::EncodePlist({{"epk", encrypted}, {"authTag", tag}}));
    std::string secret; const char* hex = "0123456789abcdef";
    for (auto c : seed) { secret += hex[c >> 4]; secret += hex[c & 15]; }
    SecureZeroMemory(seed.data(), seed.size()); return secret;
}
}
