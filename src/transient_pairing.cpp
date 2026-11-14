#include <windows.h>
#include "transient_pairing.h"
#include <openssl/bn.h>
#include <openssl/srp.h>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <algorithm>
#include <memory>
#include <stdexcept>
#include <cstring>

namespace airplay::pairing {
namespace {
void Require(bool valid) {
    if (!valid) throw std::runtime_error("Invalid transient pairing response");
}
template<class T> T Symbol(HMODULE module, const char* name) {
    auto symbol = GetProcAddress(module, name);
    if (!symbol) throw std::runtime_error("OpenSSL SRP operation is unavailable");
    static_assert(sizeof(T) == sizeof(symbol));
    T function{}; std::memcpy(&function, &symbol, sizeof(function));
    return function;
}
// Resolve arithmetic absent from upstream's OpenSSL shim. CryptoRuntime owns
// the module for the entire connection; no additional DLL search is performed.
struct Math {
    HMODULE module = GetModuleHandleW(L"libcrypto-3-x64.dll");
    decltype(&BN_CTX_new) ctxNew = Symbol<decltype(ctxNew)>(module, "BN_CTX_new");
    decltype(&BN_CTX_free) ctxFree = Symbol<decltype(ctxFree)>(module, "BN_CTX_free");
    decltype(&BN_clear_free) clear = Symbol<decltype(clear)>(module, "BN_clear_free");
    decltype(&BN_mod_exp_mont_consttime) exp = Symbol<decltype(exp)>(module, "BN_mod_exp_mont_consttime");
    decltype(&BN_mod_mul) modMul = Symbol<decltype(modMul)>(module, "BN_mod_mul");
    decltype(&BN_mod_sub) modSub = Symbol<decltype(modSub)>(module, "BN_mod_sub");
    decltype(&BN_mul) mul = Symbol<decltype(mul)>(module, "BN_mul");
    decltype(&BN_add) add = Symbol<decltype(add)>(module, "BN_add");
    decltype(&BN_cmp) cmp = Symbol<decltype(cmp)>(module, "BN_cmp");
    using Bn = std::unique_ptr<BIGNUM, decltype(clear)>;
    Bn Number(const Bytes& bytes) {
        Bn n(BN_bin2bn(bytes.data(), int(bytes.size()), nullptr), clear);
        Require(n != nullptr); return n;
    }
    Bn New() { Bn n(BN_new(), clear); Require(n != nullptr); return n; }
};
struct Sensitive {
    Bytes bytes;
    ~Sensitive() { SecureZeroMemory(bytes.data(), bytes.size()); }
};
Bytes Binary(const BIGNUM* n, int width = 0) {
    Bytes bytes(width ? width : BN_num_bytes(n));
    Require((width ? BN_bn2binpad(n, bytes.data(), width) : BN_bn2bin(n, bytes.data())) == int(bytes.size()));
    return bytes;
}
void Append(Bytes& out, const Bytes& bytes) { out.insert(out.end(), bytes.begin(), bytes.end()); }
Bytes Hash(std::initializer_list<Bytes> parts) {
    SHA512_CTX ctx{}; Bytes digest(64);
    Require(SHA512_Init(&ctx) == 1);
    for (const auto& part : parts) Require(SHA512_Update(&ctx, part.data(), part.size()) == 1);
    Require(SHA512_Final(digest.data(), &ctx) == 1);
    SecureZeroMemory(&ctx, sizeof(ctx));
    return digest;
}
using Fields = std::map<unsigned, Bytes>;
Bytes Encode(const Fields& fields) {
    Bytes out;
    for (const auto& [tag, bytes] : fields) {
        for (size_t p = 0; p < bytes.size();) {
            auto count = std::min(size_t(255), bytes.size() - p);
            out.push_back(static_cast<unsigned char>(tag)); out.push_back(static_cast<unsigned char>(count));
            out.insert(out.end(), bytes.begin() + p, bytes.begin() + p + count); p += count;
        }
    }
    return out;
}
Fields Decode(const Bytes& bytes, unsigned state) {
    Require(bytes.size() <= 4096);
    Fields fields;
    unsigned previous = 256, previousSize = 0;
    for (size_t p = 0; p < bytes.size();) {
        Require(bytes.size() - p >= 2);
        unsigned tag = bytes[p++], count = bytes[p++];
        Require(count <= bytes.size() - p);
        Require(!fields.count(tag) || (previous == tag && previousSize == 255));
        auto& value = fields[tag]; value.insert(value.end(), bytes.begin() + p, bytes.begin() + p + count);
        p += count; previous = tag; previousSize = count;
    }
    Require(!fields.count(7) && fields[6] == Bytes{static_cast<unsigned char>(state)});
    return fields;
}
}
void TransientPair(const PairingPost& post) {
    // Transient mode uses the protocol-defined PIN, not a stored user secret.
    post("/pair-pin-start", {});
    auto challenge = Decode(post("/pair-setup", Encode({{0, {0}}, {6, {1}}, {19, {16}}})), 2);
    auto salt = challenge[2], serverPublic = challenge[3];
    Require(salt.size() == 16 && !serverPublic.empty() && serverPublic.size() <= 384);
    Math math;
    std::unique_ptr<BN_CTX, decltype(math.ctxFree)> ctx(math.ctxNew(), math.ctxFree);
    Require(ctx != nullptr);
    auto group = SRP_get_default_gN("3072"); Require(group != nullptr);
    auto B = math.Number(serverPublic);
    Require(BN_num_bits(B.get()) > 0 && math.cmp(B.get(), group->N) < 0);
    Sensitive random{Bytes(32)};
    Require(RAND_bytes(random.bytes.data(), int(random.bytes.size())) == 1);
    auto a = math.Number(random.bytes), A = math.New();
    Require(math.exp(A.get(), group->g, a.get(), group->N, ctx.get(), nullptr) == 1);
    Require(BN_num_bits(A.get()) > 0);
    auto publicKey = Binary(A.get(), 384);
    auto k = math.Number(Hash({Binary(group->N, 384), Binary(group->g, 384)}));
    auto u = math.Number(Hash({publicKey, Binary(B.get(), 384)}));
    Require(BN_num_bits(u.get()) > 0);
    const Bytes username{'P','a','i','r','-','S','e','t','u','p'};
    Bytes password = username; Append(password, {':','3','9','3','9'});
    auto x = math.Number(Hash({salt, Hash({password})}));
    auto gx = math.New(), kgx = math.New(), base = math.New(), exponent = math.New(), S = math.New();
    Require(math.exp(gx.get(), group->g, x.get(), group->N, ctx.get(), nullptr) == 1);
    Require(math.modMul(kgx.get(), k.get(), gx.get(), group->N, ctx.get()) == 1);
    Require(math.modSub(base.get(), B.get(), kgx.get(), group->N, ctx.get()) == 1 && BN_num_bits(base.get()) > 0);
    Require(math.mul(exponent.get(), u.get(), x.get(), ctx.get()) == 1);
    Require(math.add(exponent.get(), exponent.get(), a.get()) == 1);
    Require(math.exp(S.get(), base.get(), exponent.get(), group->N, ctx.get(), nullptr) == 1 && BN_num_bits(S.get()) > 0);
    Sensitive shared{Binary(S.get())};
    Sensitive key{Hash({shared.bytes})};
    auto hashN = Hash({Binary(group->N)}), hashG = Hash({Binary(group->g)});
    for (size_t i = 0; i < hashN.size(); ++i) hashN[i] ^= hashG[i];
    auto proof = Hash({hashN, Hash({username}), salt, Binary(A.get()), Binary(B.get()), key.bytes});
    auto reply = Decode(post("/pair-setup", Encode({{6, {3}}, {3, publicKey}, {4, proof}})), 4);
    auto expected = Hash({Binary(A.get()), proof, key.bytes});
    Require(reply[4].size() == expected.size());
    unsigned difference = 0;
    for (size_t i = 0; i < expected.size(); ++i) difference |= expected[i] ^ reply[4][i];
    Require(difference == 0);
}
}
