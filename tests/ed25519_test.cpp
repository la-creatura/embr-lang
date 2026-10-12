// ed25519_test.cpp
// checks core/ed25519.h: SHA-512 against known digests, and Ed25519 against signatures made by another implementation (the first two
// are the RFC 8032 test vectors, the rest are other lengths of message, around the 128 byte block size). also that a changed message,
// signature or key is refused, and that random keys sign and verify.

#include <embr/embr.h>

#include <cstdio>
#include <string>
#include <vector>

using namespace embr;

static int g_failed = 0;
static void check(const char* what, bool ok) {
    if (ok) return;
    ++g_failed;
    std::printf("FAIL: %s\n", what);
}

static std::vector<unsigned char> fromHex(const std::string& h) {
    std::vector<unsigned char> out;
    for (size_t i = 0; i + 1 < h.size(); i += 2) out.push_back(static_cast<unsigned char>(std::stoi(h.substr(i, 2), nullptr, 16)));
    return out;
}
static std::string toHex(const unsigned char* d, size_t n) { return hexOf(d, n); }

struct ShaVec { size_t len; const char* hex; };
static const ShaVec kSha[] = {
    {0, "cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd47417a81a538327af927da3e"},
    {3, "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"},
    {111, "fa9121c7b32b9e01733d034cfc78cbf67f926c7ed83e82200ef86818196921760b4beff48404df811b953828274461673c68d04e297b0eb7b2b4d60fc6b566a2"},
    {112, "c01d080efd492776a1c43bd23dd99d0a2e626d481e16782e75d54c2503b5dc32bd05f0f1ba33e568b88fd2d970929b719ecbb152f58f130a407c8830604b70ca"},
    {200, "4b11459c33f52a22ee8236782714c150a3b2c60994e9acee17fe68947a3e6789f31e7668394592da7bef827cddca88c4e6f86e4df7ed1ae6cba71f3e98faee9f"},
};

struct SigVec { const char *seed, *pub, *msg, *sig; };
static const SigVec kSig[] = {
    {"9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "", "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"},
    {"4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb", "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72", "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
    {"7942bdf22106f0847762f0f3cb4d764dc7072051159a0f89f2c6dacae344bb31", "69acda0d059facb063287a8ebca5faa190439af59f7290b248fb82d31eea494e", "1245fd", "5a9470e6cf2cf7881981adc51d107104cc1cb355028e4512433d5c11b134c32ee050bf679e8fcf77f17df922eec1571b0d84898006036e3961d3af4e3f1d8508"},
    {"6f84df9ad7c5b3d076ac0e8f53a7356c88913f20f6f72db022d24d0a96dad43c", "85311563bf909f7f69a4705cf5dc8e0922a4a5b9d75fae37a4002a62ade09f88", "1617c1a98e78129e0327371065d095864f15ada0b846c1c0ebc5348adc799a", "17347cdadce1906b629fa8ac992547fd7d41d3e517d19f7184346cb2536549b1940e606274d5953026eda41fb4666c48dcbc509b15ba963d68b081398c94210d"},
    {"df849bad05d4a10ac0441eaaeeb4b48efa0b1f0abd80e998a35aba5ea0bd8799", "3401cb7fefc03fe52c634de49e80582d1380adb0a42831469a13b455ad6b576d", "c1350d439e71897aa75fde3134a4aa72e05628ac6fe68a733d1161a15d8eae2b", "8619aeadeed03c02ab89c1b8d17bad4979422133b5e198bc3fa9dacc24c3a9c19534f757a2b704eb01c1f81904bf9f09ce4bf2f07e393c8e3436c7c5e8727c0c"},
    {"b042d7958aedb1d594d6d112d34f6602f4de7110e993ae7422923d7d171165dc", "105a85d88906c01239b8b0c44fbac9a855caf1d96c734f8f40718eb6eb7d660a", "1906f63d57997a0ad31b3aae4081f41fb471653e3d577a8c4103f9cc198a7f89d81af2a5001c40173f1923f7102cfaa150a124b3c5c79bb88761a8db3f4101c2", "a01f717afa1b06235c4bbc73ca3385d0c63f7a2aa0ba5e26eca3636d6da947713e653cf02b255bc90d0069ec4de12bfaf590ccc8ae4010aebb3be46ef3ac4f03"},
    {"285b15bfebc216dc1bbefea1d7d6eb097d6f8a24d972da420ea6bf863eed3fc0", "ac77c3cfa190ce8d53c806662cc87b374eb0f1e1a764c416f6b2e5244fcbf78e", "37a33402f24978c7162f32c05b0cae3e0d3af691992d127a36331fa65c277b5c7fe8c981bccbb3d62ac078d352d4f74fcd4c5331fef7e25f4588654ba17697d3886f9d0b89f5c36658b87aa4f749d6f569ef0ef625cc17ef7578236f827b6184465f12825617a05dd82e2b3c2f879512b6e7ac030faba9dfc2f8276bfac840", "6841f8d6ed57935bf5702b3726f170369804a00e63dfab6c13cf5314a4b56ce329b237475f2c4b2dc980edb252e63b7a6e9b8d1e40c0ed57681dfdfb818bad05"},
    {"a33d8c27dd39e08031bfbce6978736ad3afcb41e965d4c5bbde83f3748a9d799", "2c04e9a5f786d770cbdad6de86fcff8137088239b317691f332561a286ad8b8c", "5feaf69f5a23365cc8b733888ac41b4515f58a7eb5aacee523b4fe394d8a3339395e60d5c8414acb63575b6780bd960fe3d0c4a19efe99f70f61013777fb58eb65636c12e339914e45ef2d190db87727ff09ada5a8b044291128af692066df71f8a13715d1276652c8fef222d86afa9b0bedeacde05ce91383bbbde5b9cd7201", "d05b64e651f5f589c75fc7b8395c9460cbfc25ddb40ffe94d2ffafa5c374457a5f3f3263339b7e3a21d32abe2aed6cc3199aa6b535921c1737aede5baf4c6409"},
    {"6b84bd49eb63516b0b57ce560e473856e2fb5e1e0bcee5a2d0101a7ace14cbfc", "5612b4f0d92a281e9892c03a64e4c020a59b5d8d5d86c17820562c93a7d32194", "0d707b30c7f26154aa3bb13f1a948cee99fa7f880facb0a22f1dde2d01350f2e095712f61b60a966f4aef5b311c39cc92c965ed33ac7abce59c5b75eb9d4e075e3f6b08956c6f9154e570bef2f31a3791c18e6eeaabd002463cc35ad9f38e6296b7b184e49053975936a70d6a360ef5a2815390c3366822b37eecc7237f8b1cee4", "edcce25ee8fe926ef9b500edc52e223add46ce968e2da278635d6439bdcb8406ba801fef77fd7d2389a9ee331d1c481be583ff982f57854fff46581c6fbf210b"},
    {"3895e3c2693b03ed9927aeb162f824bad8226d7fb31fab78dce02b806fa55469", "1d5f7bbf25d61a4745a0808ecf2c2f4b998600e81d109c7259d71cc11b85d166", "6fedd6bc61d1f7d0f01195095e310e4d961ff114636a8dfbdd13b0ef64934934e399d2e327694ef991c0be52dc9fedf271b893920fedbfb7987c0507434c0a54190068eeb5b911fa5e7a068dddad1a30e69f867effd685ad160fdb13547e45d3ac448f08561708f81eebefd4bd57965d254734d0b4e3e88e82e7904fa14713d2f876ea8b0fa23bf9408f8934de26be11f9e5639fb15cc4cba1198a6d13a2a2c8901243d580d328fd7566283a3f029023dc89f7ec8994185978f956494c5befcb0448c81b5c5a9f61424b184d6dc336ddc75d0d8f35433a4a9740c4b4276203bd48f47d20b5f835a0f20ab0e2d0ed9ce24ce9c466975b9955a28867421b1dd15b3a07503ecebf8c2fede1a64a6fa5e9bfa2b7b0ae92988a5d3f71ae7f90de88e742f4ab5ae21a23d5d9951e79", "5f2b11f078881d8c1dd46c08e67a72345029512be3a5372552513aaa92a8b2add61f523e5bb1e34c3dfbf112356030e3d7f99632ba89a4526f94bf9966ced70a"},
    {"c3c46c26bb6d1cfc3cdcc7b90699bebdcce0be35fd4aa57000bd200047296ba4", "73dca8455b76b6a9eb6ca4bbb3f96b5848cbca560535bd2a9d096a9fe346e6f5", "de90640f0ea0e3ba6de1ad3dc173f3479d92613c582bdc0eb3c303fa5ef28c47c768dd98df92312a20e2a42104a6f5d830a9d673a567c82d1a0d7a2b5c76f0ca91b0e9746798af44bab5a168e41edd9f63fc6e5e35e93dcb6ca15f13a7ff7ec41d79cdbbc7725a9180b0861bae386a709be258567df76b6eba7659c9d95a9ae2bf1c28eafa095a89d6fd71c7f8b1cff75b3ad6ad49a437b24b98f44de4bffc15b1672f9b93a5d29505d8b3d8f5bd5c7f9760c538a552a3f858c9ee64d1bb361cf66754553d333cc6a1cb8c21f58da075853c6c38f4bede4b86bd5f8766727e84b1af3632452a73eaaa3aa7485415fb8944bee1dbdadc83978bc64e171057dc006c436abe8316b7673c516f25f8db934c3bac5284462c284630032ac8e9e2865a012390d4570310aca7b934166cc605e3a772e1e77902b06e7550d4c831caa13a7c94310ccf0dad7264e887c36bda9d205be733914be0409d5673bb973e25ff22f57ed40a050bf2021df50618b0979cf1fb999c439e2dcf163a62792a51c847542a9a03fd22b9ff2bd7aac8deeeaf0ea59cf684f28369b73186f5d7f467ac1d7a18cd7bc55ca3325be782dcd7aec1dc14bac9bac88b704674640fd335cdf4f4c1dd66f2ca1d520b633146ca3b3e356d9641d8de4a506ed8fea1e20e8209afa2c56d3e90652b69c77c71aa752faa9b6e6acb00987a4a8e2480b068944fb06e663be8da801cca879366e6689fb22d36d7ccb079803f3e0c58a3b05672a7183aa222d0ab940af9e2e056f7003d57bdf58ec365ebc0f2b9bb05a8f5fde705f1259c5f773a585985a028b90271aa071bc25436e7b2f804ebde29372e2b67358f2aaa0e6b19e1133f6da7e39ba40762076fde41b2e3d2f1459b90277754aae0c77ba295acb05797be255ec59be9cfdc6e33b303bae11cea80fdfb62982c25f8e2ca5467864486005b6679e83c94bc623300a6e19afa16a3965bebfe9ff3af7b206712ba25b1035b13f250a73057ba7af31f0575a348927fb2b2c04ce8dbcf1dedce0b2988d69cba3f045c30d016cad87f27de1587bc5edbb2d30a77b677c4bdb5935174ba007d50f8866346f2e254cf82c3c82f69c1366bb9b6849bc1de4eb02692a48e6362cb6d6e77bf86e57e3c3809d3109f9fb640438358f2c5857a52f52a63b506f91ed7dd3bc8a1d29ab5cd27ed632481e03a872278b28e7ea5d01926004b64733666b5562c56cae90b35a56ac3d47137172d229b645de35e2cb325f84e487973f88ecf1f7699b52358510de53879375844a16cd90c962de07feb793dcf5273883240efd10dc53a78d6542d27f20f5979a5aaf0eb952737b71f0ab7a112225b843d0d90f003d7c41980af4fb46822a499c24fc7832a6dfa2ec79c24a17e6371c9ed94494e017b58e8", "a5e033f6c1c952bcf39c6d409e36d7b240898a18a42bf0421bcdf1c8ee20c335bd41979d796efbd760ee5adb6c03dcaf069b66db41543952cfb6c95c222eaa01"},
};

int main() {
    for (const auto& v : kSha) {
        std::string msg(v.len == 3 ? "abc" : std::string(v.len, 'a'));
        if (v.len == 0) msg = "";
        unsigned char d[64];
        ed25519::sha512(reinterpret_cast<const unsigned char*>(msg.data()), msg.size(), d);
        check(("sha512 of " + std::to_string(v.len) + " bytes").c_str(), toHex(d, 64) == v.hex);
    }
    int n = 0;
    for (const auto& v : kSig) {
        ++n;
        const std::string tag = "vector " + std::to_string(n) + ": ";
        auto seed = fromHex(v.seed), pub = fromHex(v.pub), msg = fromHex(v.msg), sig = fromHex(v.sig);
        unsigned char gotPub[32], sec[64], gotSig[64];
        ed25519::keypair(seed.data(), gotPub, sec);
        check((tag + "public key").c_str(), toHex(gotPub, 32) == v.pub);
        ed25519::sign(msg.data(), msg.size(), sec, gotSig);
        check((tag + "signature").c_str(), toHex(gotSig, 64) == v.sig);
        check((tag + "verifies").c_str(), ed25519::verify(msg.data(), msg.size(), sig.data(), pub.data()));
        // anything changed is refused
        for (size_t bit : {size_t(0), size_t(7), size_t(255), size_t(300), size_t(511)}) {
            auto bad = sig;
            bad[(bit / 8) % 64] ^= static_cast<unsigned char>(1u << (bit % 8));
            check((tag + "a changed signature").c_str(), !ed25519::verify(msg.data(), msg.size(), bad.data(), pub.data()));
        }
        if (!msg.empty()) {
            auto bad = msg;
            bad[bad.size() / 2] ^= 1;
            check((tag + "a changed message").c_str(), !ed25519::verify(bad.data(), bad.size(), sig.data(), pub.data()));
        }
        auto badPub = pub;
        badPub[5] ^= 1;
        check((tag + "a changed key").c_str(), !ed25519::verify(msg.data(), msg.size(), sig.data(), badPub.data()));
        auto other = msg;
        other.push_back(0);
        check((tag + "a longer message").c_str(), !ed25519::verify(other.data(), other.size(), sig.data(), pub.data()));
    }
    // random keys
    for (int i = 0; i < 20; ++i) {
        unsigned char seed[32], pub[32], sec[64], sig[64], msg[48];
        check("random bytes", ed25519::randomBytes(seed, 32) && ed25519::randomBytes(msg, sizeof msg));
        ed25519::keypair(seed, pub, sec);
        ed25519::sign(msg, sizeof msg, sec, sig);
        check("a signature made here verifies here", ed25519::verify(msg, sizeof msg, sig, pub));
        msg[3] ^= 1;
        check("and not for another message", !ed25519::verify(msg, sizeof msg, sig, pub));
    }
    // garbage in the key place must be refused, not crash
    unsigned char zero[32] = {0}, ones[32], sig[64] = {0}, m[1] = {0};
    std::memset(ones, 0xff, sizeof ones);
    check("an all zero key and signature", !ed25519::verify(m, 0, sig, zero));
    check("an all one key", !ed25519::verify(m, 1, sig, ones));
    if (g_failed) { std::printf("%d failed\n", g_failed); return 1; }
    std::printf("ed25519 test OK\n");
    return 0;
}
