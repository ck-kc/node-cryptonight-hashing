#include <node.h>
#include <node_buffer.h>
#include <v8.h>
#include <stdint.h>
#include <nan.h>
#include <stdexcept>

#if defined(__ARM_ARCH)
  #define my_malloc(a, b) malloc(a)
#else 
  #define my_malloc(a, b) _mm_malloc(a, b)
#endif

//#if (defined(__AES__) && (__AES__ == 1)) || defined(__APPLE__) || defined(__ARM_ARCH)
//#else
//#define _mm_aeskeygenassist_si128(a, b) a
//#define _mm_aesenc_si128(a, b) a
//#endif

#include "crypto/common/VirtualMemory.h"
#include "crypto/cn/CnCtx.h" 
#include "crypto/cn/CnHash.h"
#include "crypto/randomx/configuration.h"
#include "crypto/randomx/randomx.h"
#include "crypto/astrobwt/AstroBWT.h"
#include "crypto/kawpow/KPHash.h"
#include "3rdparty/libethash/ethash.h"
#include "crypto/ghostrider/ghostrider.h"
#include "crypto/flex/flex.h"
#include "crypto/ghostrider/sph_keccak.h"

extern "C" {
#include "crypto/randomx/panthera/KangarooTwelve.h"
#include "crypto/randomx/blake2/blake2.h"
#include "c29/portable_endian.h" // for htole32/64
#include "c29/int-util.h"
}

#include "c29.h"

#if (defined(__AES__) && (__AES__ == 1)) || (defined(__ARM_FEATURE_CRYPTO) && (__ARM_FEATURE_CRYPTO == 1))
  #define SOFT_AES false
  #if defined(CPU_INTEL)
    #warning Using IvyBridge assembler implementation
    #define ASM_TYPE xmrig::Assembly::INTEL
  #elif defined(CPU_AMD)
    #warning Using Ryzen assembler implementation
    #define ASM_TYPE xmrig::Assembly::RYZEN
  #elif defined(CPU_AMD_OLD)
    #warning Using Bulldozer assembler implementation
    #define ASM_TYPE xmrig::Assembly::BULLDOZER
  #elif !defined(__ARM_ARCH)
    #error Unknown ASM implementation!
  #endif
#else
  #warning Using software AES
  #define SOFT_AES true
#endif

#define FN(algo)  xmrig::CnHash::fn(xmrig::Algorithm::algo, SOFT_AES ? xmrig::CnHash::AV_SINGLE_SOFT : xmrig::CnHash::AV_SINGLE, xmrig::Assembly::NONE)
#if defined(ASM_TYPE)
  #define FNA(algo) xmrig::CnHash::fn(xmrig::Algorithm::algo, SOFT_AES ? xmrig::CnHash::AV_SINGLE_SOFT : xmrig::CnHash::AV_SINGLE, ASM_TYPE)
#else
  #define FNA(algo) xmrig::CnHash::fn(xmrig::Algorithm::algo, SOFT_AES ? xmrig::CnHash::AV_SINGLE_SOFT : xmrig::CnHash::AV_SINGLE, xmrig::Assembly::NONE)
#endif


const size_t max_mem_size = 20 * 1024 * 1024;
xmrig::VirtualMemory mem(max_mem_size, true, false, 0, 4096);
static struct cryptonight_ctx* ctx = nullptr;

const int MAXRX = 4;
int rx2id(xmrig::Algorithm::Id algo) {
  switch (algo) {
      case xmrig::Algorithm::RX_0:     return 0;
      case xmrig::Algorithm::RX_ARQ:   return 1;
      case xmrig::Algorithm::RX_XEQ:   return 2;
      //case xmrig::Algorithm::RX_WOW:   return 2;
      //case xmrig::Algorithm::RX_GRAFT: return 3;
      //case xmrig::Algorithm::RX_SFX:   return 4;
      //case xmrig::Algorithm::RX_KEVA:  return 5;
      case xmrig::Algorithm::RX_XLA:   return MAXRX-1;
      default: return 0;
  }
}

static randomx_vm*    rx_vm[MAXRX]            = {nullptr};

const int rx_seed_cache_size = MAXRX+2;
static randomx_cache* rx_cache[rx_seed_cache_size]         = {nullptr};
static uint8_t        rx_seed_hash[rx_seed_cache_size][32] = {0};

struct InitCtx {
    InitCtx() {
        xmrig::CnCtx::create(&ctx, static_cast<uint8_t*>(my_malloc(max_mem_size, 4096)), max_mem_size, 1);
        for (int i = 0; i != rx_seed_cache_size; ++ i) memset(rx_seed_hash[i], 0xCC, sizeof(rx_seed_hash[0]));
    }
} s;

void init_rx(const uint8_t* seed_hash_data, xmrig::Algorithm::Id algo) {
    bool update_cache = false;
    const int rxid = rx2id(algo);
    assert(rxid < MAXRX);

    randomx_set_scratchpad_prefetch_mode(0);
    randomx_set_huge_pages_jit(false);
    //randomx_set_optimized_dataset_init(0);

    switch (algo) {
        case xmrig::Algorithm::RX_0:
            randomx_apply_config(RandomX_MoneroConfig);
            break;
        //case xmrig::Algorithm::RX_WOW:
        //    randomx_apply_config(RandomX_WowneroConfig);
        //    break;
        case xmrig::Algorithm::RX_ARQ:
            randomx_apply_config(RandomX_ArqmaConfig);
            break;
        case xmrig::Algorithm::RX_XEQ:
            randomx_apply_config(RandomX_EquilibriaConfig);
            break;
        //case xmrig::Algorithm::RX_GRAFT:
        //    randomx_apply_config(RandomX_GraftConfig);
        //    break;
        //case xmrig::Algorithm::RX_KEVA:
        //    randomx_apply_config(RandomX_KevaConfig);
        //    break;
        case xmrig::Algorithm::RX_XLA:
            randomx_apply_config(RandomX_ScalaConfig);
            break;
        default:
            throw std::domain_error("Unknown RandomX algo");
    }

    int found_rxid = -1;
    for (int i = 0; i != rx_seed_cache_size; ++ i)
      if (memcmp(rx_seed_hash[i], seed_hash_data, sizeof(rx_seed_hash[0])) == 0) {
        found_rxid = i;
        break;
      }

    if (found_rxid == -1) {
        static int new_rxid = 0;
	if (rx_cache[new_rxid] == nullptr) {
          uint8_t* const pmem = static_cast<uint8_t*>(my_malloc(RANDOMX_CACHE_MAX_SIZE, 4096));
          rx_cache[new_rxid] = randomx_create_cache(RANDOMX_FLAG_JIT, pmem);
        } else {
          randomx_release_cache(rx_cache[new_rxid]);
        }
        memcpy(rx_seed_hash[new_rxid], seed_hash_data, sizeof(rx_seed_hash[0]));
        randomx_init_cache(rx_cache[new_rxid], rx_seed_hash[new_rxid], sizeof(rx_seed_hash[0]));
        found_rxid = new_rxid;
        ++ new_rxid;
        if (new_rxid >= rx_seed_cache_size) new_rxid = 0;
    }

    if (rx_vm[rxid] == nullptr) {
        int flags = 0;
#if !defined(__ARM_ARCH)
        flags |= RANDOMX_FLAG_JIT;
#endif
#if !SOFT_AES
        flags |= RANDOMX_FLAG_HARD_AES;
#endif
        rx_vm[rxid] = randomx_create_vm(static_cast<randomx_flags>(flags), rx_cache[found_rxid], nullptr, mem.scratchpad(), 0);
    } else {
        randomx_vm_set_cache(rx_vm[rxid], rx_cache[found_rxid]);
    }
}

#define THROW_ERROR_EXCEPTION(x) Nan::ThrowError(x)

void callback(char* data, void* hint) {
    free(data);
}

using namespace node;
using namespace v8;
using namespace Nan;

NAN_METHOD(randomx) {
    if (info.Length() < 2) return THROW_ERROR_EXCEPTION("You must provide two arguments.");

    v8::Isolate *isolate = v8::Isolate::GetCurrent();
    Local<Object> target = info[0]->ToObject(isolate->GetCurrentContext()).ToLocalChecked();
    if (!Buffer::HasInstance(target)) return THROW_ERROR_EXCEPTION("Argument 1 should be a buffer object.");

    Local<Object> seed_hash = info[1]->ToObject(isolate->GetCurrentContext()).ToLocalChecked();
    if (!Buffer::HasInstance(seed_hash)) return THROW_ERROR_EXCEPTION("Argument 2 should be a buffer object.");
    if (Buffer::Length(seed_hash) != sizeof(rx_seed_hash[0])) return THROW_ERROR_EXCEPTION("Argument 2 size should be 32 bytes.");

    int algo = 0;
    if (info.Length() >= 3) {
        if (!info[2]->IsNumber()) return THROW_ERROR_EXCEPTION("Argument 3 should be a number");
        algo = Nan::To<int>(info[2]).FromMaybe(0);
    }

    xmrig::Algorithm xalgo;
    switch (algo) {
        case 0:  xalgo = xmrig::Algorithm::RX_0; break;
        //case 1:  xalgo = xmrig::Algorithm::RX_DEFYX; break;
        case 2:  xalgo = xmrig::Algorithm::RX_ARQ; break;
        case 3:  xalgo = xmrig::Algorithm::RX_XLA; break;
        //case 17: xalgo = xmrig::Algorithm::RX_WOW; break;
        //case 18: xalgo = xmrig::Algorithm::RX_LOKI; break;
        //case 19: xalgo = xmrig::Algorithm::RX_KEVA; break;
        //case 20: xalgo = xmrig::Algorithm::RX_GRAFT; break;
        case 22: xalgo = xmrig::Algorithm::RX_XEQ; break;
        default: xalgo = xmrig::Algorithm::RX_0;
    }

    try {
        init_rx(reinterpret_cast<const uint8_t*>(Buffer::Data(seed_hash)), xalgo);
    } catch (const std::domain_error &e) {
        return THROW_ERROR_EXCEPTION(e.what());
    }

    char output[32];
    randomx_calculate_hash(rx_vm[rx2id(xalgo)], reinterpret_cast<const uint8_t*>(Buffer::Data(target)), Buffer::Length(target), reinterpret_cast<uint8_t*>(output), xalgo);

    v8::Local<v8::Value> returnValue = Nan::CopyBuffer(output, 32).ToLocalChecked();
    info.GetReturnValue().Set(returnValue);
}

void ghostrider(const unsigned char* data, long unsigned int size, unsigned char* output, cryptonight_ctx** ctx, long unsigned int) {
    xmrig::ghostrider::hash(data, size, output, ctx, nullptr);
}

void flex(const unsigned char* data, long unsigned int size, unsigned char* output, cryptonight_ctx** ctx, long unsigned int) {
    hard_coded_eb = 6;
    flex_hash((const char*)data, (char*)output, ctx);
    hard_coded_eb = 1;
}

static xmrig::cn_hash_fun get_cn_fn(const int algo) {
  switch (algo) {
    case 0:  return FN(CN_0);
    case 1:  return FN(CN_1);
    case 4:  return FN(CN_FAST);
    case 6:  return FN(CN_XAO);
    case 7:  return FN(CN_RTO);
    case 8:  return FNA(CN_2);
    case 9:  return FNA(CN_HALF);
    case 11: return FN(CN_GPU);
    case 13: return FNA(CN_R);
    case 14: return FNA(CN_RWZ);
    case 15: return FNA(CN_ZLS);
    case 16: return FNA(CN_DOUBLE);
    case 17: return FNA(CN_CCX);
    case 18: return ghostrider;
    case 19: return flex;
    
    default: return FN(CN_R);
  }
}

static xmrig::cn_hash_fun get_cn_lite_fn(const int algo) {
  switch (algo) {
    case 0:  return FN(CN_LITE_0);
    case 1:  return FN(CN_LITE_1);
    default: return FN(CN_LITE_1);
  }
}

static xmrig::cn_hash_fun get_cn_heavy_fn(const int algo) {
  switch (algo) {
    case 0:  return FN(CN_HEAVY_0);
    case 1:  return FN(CN_HEAVY_XHV);
    case 2:  return FN(CN_HEAVY_TUBE);
    default: return FN(CN_HEAVY_0);
  }
}

static xmrig::cn_hash_fun get_cn_pico_fn(const int algo) {
  switch (algo) {
    case 0:  return FNA(CN_PICO_0);
    default: return FNA(CN_PICO_0);
  }
}

static xmrig::cn_hash_fun get_cn_femto_fn(const int algo) {
  switch (algo) {
    case 0:  return FN(CN_UPX2);
    case 1:  return FN(CN_UPX2);
    case 2:  return FN(CN_UPX2);
	  case 3:  return FN(CN_UPX2);
	  case 4:  return FN(CN_UPX2);
	  case 5:  return FN(CN_UPX2);
	  case 6:  return FN(CN_UPX2);
	  case 7:  return FN(CN_UPX2);
	  case 8:  return FN(CN_UPX2);
	  case 9:  return FN(CN_UPX2);
	  case 10:  return FN(CN_UPX2);
	  case 11:  return FN(CN_UPX2);
	  case 12:  return FN(CN_UPX2);
	  case 13:  return FN(CN_UPX2);
	  case 14:  return FN(CN_UPX2);
	  case 15:  return FN(CN_UPX2);
	  case 16:  return FN(CN_UPX2);
	  case 17:  return FN(CN_UPX2);
	  case 18:  return FN(CN_UPX2);
	  case 19:  return FN(CN_UPX2);
	  case 20:  return FN(CN_UPX2);
    default: return FN(CN_UPX2);
  }
}

static xmrig::cn_hash_fun get_argon2_fn(const int algo) {
  switch (algo) {
    case 0:  return FN(AR2_CHUKWA);
    case 1:  return FN(AR2_WRKZ);
    case 2:  return FN(AR2_CHUKWA_V2);
    default: return FN(AR2_CHUKWA);
  }
}

static xmrig::cn_hash_fun get_astrobwt_fn(const int algo) {
  switch (algo) {
    case 0:  return FN(ASTROBWT_DERO);
    case 1:  return FN(ASTROBWT_DERO_2);
    default: return FN(ASTROBWT_DERO);
  }
}

NAN_METHOD(cryptonight) {
    if (info.Length() < 1) return THROW_ERROR_EXCEPTION("You must provide one argument.");

    v8::Isolate *isolate = v8::Isolate::GetCurrent();
    Local<Object> target = info[0]->ToObject(isolate->GetCurrentContext()).ToLocalChecked();
    if (!Buffer::HasInstance(target)) return THROW_ERROR_EXCEPTION("Argument 1 should be a buffer object.");

    int algo = 0;
    uint64_t height = 0;
    bool height_set = false;

    if (info.Length() >= 2) {
        if (!info[1]->IsNumber()) return THROW_ERROR_EXCEPTION("Argument 2 should be a number");
        algo = Nan::To<int>(info[1]).FromMaybe(0);
    }

    if (info.Length() >= 3) {
        if (!info[2]->IsNumber()) return THROW_ERROR_EXCEPTION("Argument 3 should be a number");
        height = Nan::To<uint32_t>(info[2]).FromMaybe(0);
        height_set = true;
    }

    if ((algo == 12 || algo == 13) && !height_set) return THROW_ERROR_EXCEPTION("CryptonightR requires block template height as Argument 3");

    const xmrig::cn_hash_fun fn = get_cn_fn(algo);

    char output[32];
    fn(reinterpret_cast<const uint8_t*>(Buffer::Data(target)), Buffer::Length(target), reinterpret_cast<uint8_t*>(output), &ctx, height);

    v8::Local<v8::Value> returnValue = Nan::CopyBuffer(output, 32).ToLocalChecked();
    info.GetReturnValue().Set(returnValue);
}

NAN_METHOD(cryptonight_plex) {
    if (info.Length() < 1) return THROW_ERROR_EXCEPTION("You must provide one argument.");

    v8::Isolate *isolate = v8::Isolate::GetCurrent();
    Local<Object> target = info[0]->ToObject(isolate->GetCurrentContext()).ToLocalChecked();
    if (!Buffer::HasInstance(target)) return THROW_ERROR_EXCEPTION("Argument 1 should be a buffer object.");

    int algo = 0;

    if (info.Length() >= 2) {
        if (!info[1]->IsNumber()) return THROW_ERROR_EXCEPTION("Argument 2 should be a number");
        algo = Nan::To<int>(info[1]).FromMaybe(0);
    }

    const xmrig::cn_hash_fun fn = get_cn_femto_fn(algo);

    char output[32];
    fn(reinterpret_cast<const uint8_t*>(Buffer::Data(target)), Buffer::Length(target), reinterpret_cast<uint8_t*>(output), &ctx, 0);

    v8::Local<v8::Value> returnValue = Nan::CopyBuffer(output, 32).ToLocalChecked();
    info.GetReturnValue().Set(returnValue);
}

NAN_METHOD(cryptonight_light) {
    if (info.Length() < 1) return THROW_ERROR_EXCEPTION("You must provide one argument.");

    v8::Isolate *isolate = v8::Isolate::GetCurrent();
    Local<Object> target = info[0]->ToObject(isolate->GetCurrentContext()).ToLocalChecked();
    if (!Buffer::HasInstance(target)) return THROW_ERROR_EXCEPTION("Argument 1 should be a buffer object.");

    int algo = 0;
    uint64_t height = 0;

    if (info.Length() >= 2) {
        if (!info[1]->IsNumber()) return THROW_ERROR_EXCEPTION("Argument 2 should be a number");
        algo = Nan::To<int>(info[1]).FromMaybe(0);
    }

    if (info.Length() >= 3) {
        if (!info[2]->IsNumber()) return THROW_ERROR_EXCEPTION("Argument 3 should be a number");
        height = Nan::To<unsigned int>(info[2]).FromMaybe(0);
    }

    const xmrig::cn_hash_fun fn = get_cn_lite_fn(algo);

    char output[32];
    fn(reinterpret_cast<const uint8_t*>(Buffer::Data(target)), Buffer::Length(target), reinterpret_cast<uint8_t*>(output), &ctx, height);

    v8::Local<v8::Value> returnValue = Nan::CopyBuffer(output, 32).ToLocalChecked();
    info.GetReturnValue().Set(returnValue);
}

NAN_METHOD(cryptonight_heavy) {
    if (info.Length() < 1) return THROW_ERROR_EXCEPTION("You must provide one argument.");

    v8::Isolate *isolate = v8::Isolate::GetCurrent();
    Local<Object> target = info[0]->ToObject(isolate->GetCurrentContext()).ToLocalChecked();
    if (!Buffer::HasInstance(target)) return THROW_ERROR_EXCEPTION("Argument 1 should be a buffer object.");

    int algo = 0;
    uint64_t height = 0;

    if (info.Length() >= 2) {
        if (!info[1]->IsNumber()) return THROW_ERROR_EXCEPTION("Argument 2 should be a number");
        algo = Nan::To<int>(info[1]).FromMaybe(0);
    }

    if (info.Length() >= 3) {
        if (!info[2]->IsNumber()) return THROW_ERROR_EXCEPTION("Argument 3 should be a number");
        height = Nan::To<unsigned int>(info[2]).FromMaybe(0);
    }


    const xmrig::cn_hash_fun fn = get_cn_heavy_fn(algo);

    char output[32];
    fn(reinterpret_cast<const uint8_t*>(Buffer::Data(target)), Buffer::Length(target), reinterpret_cast<uint8_t*>(output), &ctx, height);

    v8::Local<v8::Value> returnValue = Nan::CopyBuffer(output, 32).ToLocalChecked();
    info.GetReturnValue().Set(returnValue);
}

NAN_METHOD(cryptonight_pico) {
    if (info.Length() < 1) return THROW_ERROR_EXCEPTION("You must provide one argument.");

    v8::Isolate *isolate = v8::Isolate::GetCurrent();
    Local<Object> target = info[0]->ToObject(isolate->GetCurrentContext()).ToLocalChecked();
    if (!Buffer::HasInstance(target)) return THROW_ERROR_EXCEPTION("Argument 1 should be a buffer object.");

    int algo = 0;

    if (info.Length() >= 2) {
        if (!info[1]->IsNumber()) return THROW_ERROR_EXCEPTION("Argument 2 should be a number");
        algo = Nan::To<int>(info[1]).FromMaybe(0);
    }

    const xmrig::cn_hash_fun fn = get_cn_pico_fn(algo);

    char output[32];
    fn(reinterpret_cast<const uint8_t*>(Buffer::Data(target)), Buffer::Length(target), reinterpret_cast<uint8_t*>(output), &ctx, 0);

    v8::Local<v8::Value> returnValue = Nan::CopyBuffer(output, 32).ToLocalChecked();
    info.GetReturnValue().Set(returnValue);
}

NAN_METHOD(argon2) {
    if (info.Length() < 1) return THROW_ERROR_EXCEPTION("You must provide one argument.");

    v8::Isolate *isolate = v8::Isolate::GetCurrent();
    Local<Object> target = info[0]->ToObject(isolate->GetCurrentContext()).ToLocalChecked();
    if (!Buffer::HasInstance(target)) return THROW_ERROR_EXCEPTION("Argument 1 should be a buffer object.");

    int algo = 0;

    if (info.Length() >= 2) {
        if (!info[1]->IsNumber()) return THROW_ERROR_EXCEPTION("Argument 2 should be a number");
        algo = Nan::To<int>(info[1]).FromMaybe(0);
    }

    const xmrig::cn_hash_fun fn = get_argon2_fn(algo);

    char output[32];
    fn(reinterpret_cast<const uint8_t*>(Buffer::Data(target)), Buffer::Length(target), reinterpret_cast<uint8_t*>(output), &ctx, 0);

    v8::Local<v8::Value> returnValue = Nan::CopyBuffer(output, 32).ToLocalChecked();
    info.GetReturnValue().Set(returnValue);
}

NAN_METHOD(astrobwt) {
    if (info.Length() < 1) return THROW_ERROR_EXCEPTION("You must provide one argument.");

    v8::Isolate *isolate = v8::Isolate::GetCurrent();
    Local<Object> target = info[0]->ToObject(isolate->GetCurrentContext()).ToLocalChecked();
    if (!Buffer::HasInstance(target)) return THROW_ERROR_EXCEPTION("Argument 1 should be a buffer object.");

    int algo = 0;

    if (info.Length() >= 2) {
        if (!info[1]->IsNumber()) return THROW_ERROR_EXCEPTION("Argument 2 should be a number");
        algo = Nan::To<int>(info[1]).FromMaybe(0);
    }

    const xmrig::cn_hash_fun fn = get_astrobwt_fn(algo);

    char output[32];
    fn(reinterpret_cast<const uint8_t*>(Buffer::Data(target)), Buffer::Length(target), reinterpret_cast<uint8_t*>(output), &ctx, 0);

    v8::Local<v8::Value> returnValue = Nan::CopyBuffer(output, 32).ToLocalChecked();
    info.GetReturnValue().Set(returnValue);
}

NAN_METHOD(k12) {
    if (info.Length() < 1) return THROW_ERROR_EXCEPTION("You must provide one argument.");

    v8::Isolate *isolate = v8::Isolate::GetCurrent();
    Local<Object> target = info[0]->ToObject(isolate->GetCurrentContext()).ToLocalChecked();
	
    if (!Buffer::HasInstance(target)) return THROW_ERROR_EXCEPTION("Argument 1 should be a buffer object.");

    char output[32];
    KangarooTwelve((const unsigned char *)Buffer::Data(target), Buffer::Length(target), (unsigned char *)output, 32, 0, 0);

    v8::Local<v8::Value> returnValue = Nan::CopyBuffer(output, 32).ToLocalChecked();
    info.GetReturnValue().Set(returnValue);
}

static void setsipkeys(const char *keybuf,siphash_keys *keys) {
	keys->k0 = htole64(((uint64_t *)keybuf)[0]);
	keys->k1 = htole64(((uint64_t *)keybuf)[1]);
	keys->k2 = htole64(((uint64_t *)keybuf)[2]);
	keys->k3 = htole64(((uint64_t *)keybuf)[3]);
}

static void c29_setheader(const char *header, const uint32_t headerlen, siphash_keys *keys) {
	char hdrkey[32];
	rx_blake2b((void *)hdrkey, sizeof(hdrkey), (const void *)header, headerlen);
	setsipkeys(hdrkey,keys);
}

NAN_METHOD(c29s) {
	if (info.Length() != 2) return THROW_ERROR_EXCEPTION("You must provide 2 arguments: header, ring");
	
	char * input = Buffer::Data(info[0]);
	uint32_t input_len = Buffer::Length(info[0]);

	siphash_keys keys;
	c29_setheader(input,input_len,&keys);
	
	Local<Array> ring = Local<Array>::Cast(info[1]);

	uint32_t edges[PROOFSIZE];
	for (uint32_t n = 0; n < PROOFSIZE; n++)
		edges[n]=ring->Get(Nan::GetCurrentContext(), n).ToLocalChecked()->Uint32Value(Nan::GetCurrentContext()).FromJust();
	
	int retval = c29s_verify(edges,&keys);

	info.GetReturnValue().Set(Nan::New<Number>(retval));
}

NAN_METHOD(c29v) {
	if (info.Length() != 2) return THROW_ERROR_EXCEPTION("You must provide 2 arguments: header, ring");
	
	char * input = Buffer::Data(info[0]);
	uint32_t input_len = Buffer::Length(info[0]);

	siphash_keys keys;
	c29_setheader(input,input_len,&keys);

	Local<Array> ring = Local<Array>::Cast(info[1]);

	uint32_t edges[PROOFSIZE];
	for (uint32_t n = 0; n < PROOFSIZE; n++)
		edges[n]=ring->Get(Nan::GetCurrentContext(), n).ToLocalChecked()->Uint32Value(Nan::GetCurrentContext()).FromJust();
	
	int retval = c29v_verify(edges,&keys);

	info.GetReturnValue().Set(Nan::New<Number>(retval));
}

NAN_METHOD(c29i) {
	if (info.Length() != 2) return THROW_ERROR_EXCEPTION("You must provide 2 arguments: header, ring");
	
	char * input = Buffer::Data(info[0]);
	uint32_t input_len = Buffer::Length(info[0]);

	siphash_keys keys;
	c29_setheader(input,input_len,&keys);
	
	Local<Array> ring = Local<Array>::Cast(info[1]);

	uint32_t edges[PROOFSIZEi];
	for (uint32_t n = 0; n < PROOFSIZEi; n++)
		edges[n]=ring->Get(Nan::GetCurrentContext(), n).ToLocalChecked()->Uint32Value(Nan::GetCurrentContext()).FromJust();
	
	int retval = c29i_verify(edges,&keys);

	info.GetReturnValue().Set(Nan::New<Number>(retval));
}

NAN_METHOD(c29b) {
	if (info.Length() != 2) return THROW_ERROR_EXCEPTION("You must provide 2 arguments: header, ring");
	
	char * input = Buffer::Data(info[0]);
	uint32_t input_len = Buffer::Length(info[0]);

	siphash_keys keys;
	c29_setheader(input,input_len,&keys);
	
	Local<Array> ring = Local<Array>::Cast(info[1]);

	uint32_t edges[PROOFSIZEb];
	for (uint32_t n = 0; n < PROOFSIZEb; n++)
		edges[n]=ring->Get(Nan::GetCurrentContext(), n).ToLocalChecked()->Uint32Value(Nan::GetCurrentContext()).FromJust();
	
	int retval = c29b_verify(edges,&keys);

	info.GetReturnValue().Set(Nan::New<Number>(retval));
}


NAN_METHOD(c29_cycle_hash) {
	if (info.Length() != 1) return THROW_ERROR_EXCEPTION("You must provide 1 argument:ring");
	
	Local<Array> ring = Local<Array>::Cast(info[0]);

	uint8_t hashdata[116]; // PROOFSIZE*EDGEBITS/8
	memset(hashdata, 0, 116);

	int bytepos = 0;
	int bitpos = 0;
	for(int i = 0; i < PROOFSIZE; i++){

		uint32_t node = ring->Get(Nan::GetCurrentContext(), i).ToLocalChecked()->Uint32Value(Nan::GetCurrentContext()).FromJust();

		for(int j = 0; j < EDGEBITS; j++) {
			
			if((node >> j) & 1U)
				hashdata[bytepos] |= 1UL << bitpos;

			bitpos++;
			if(bitpos==8) {
				bitpos=0;bytepos++;
			}
		}
	}

	unsigned char cyclehash[32];
	rx_blake2b((void *)cyclehash, sizeof(cyclehash), (uint8_t *)hashdata, sizeof(hashdata));
	
	unsigned char rev_cyclehash[32];
	for(int i = 0; i < 32; i++)
		rev_cyclehash[i] = cyclehash[31-i];
	
	v8::Local<v8::Value> returnValue = Nan::CopyBuffer((char*)rev_cyclehash, 32).ToLocalChecked();
	info.GetReturnValue().Set(returnValue);
}

NAN_METHOD(c29b_cycle_hash) {
	if (info.Length() != 1) return THROW_ERROR_EXCEPTION("You must provide 1 argument:ring");
	
	Local<Array> ring = Local<Array>::Cast(info[0]);

	uint8_t hashdata[145]; // PROOFSIZEb*EDGEBITS/8
	memset(hashdata, 0, 145);

	int bytepos = 0;
	int bitpos = 0;
	for(int i = 0; i < PROOFSIZEb; i++){

		uint32_t node = ring->Get(Nan::GetCurrentContext(), i).ToLocalChecked()->Uint32Value(Nan::GetCurrentContext()).FromJust();

		for(int j = 0; j < EDGEBITS; j++) {
			
			if((node >> j) & 1U)
				hashdata[bytepos] |= 1UL << bitpos;

			bitpos++;
			if(bitpos==8) {
				bitpos=0;bytepos++;
			}
		}
	}

	unsigned char cyclehash[32];
	rx_blake2b((void *)cyclehash, sizeof(cyclehash), (uint8_t *)hashdata, sizeof(hashdata));
	
	unsigned char rev_cyclehash[32];
	for(int i = 0; i < 32; i++)
		rev_cyclehash[i] = cyclehash[31-i];
	
	v8::Local<v8::Value> returnValue = Nan::CopyBuffer((char*)rev_cyclehash, 32).ToLocalChecked();
	info.GetReturnValue().Set(returnValue);
}

NAN_METHOD(c29i_cycle_hash) {
	if (info.Length() != 1) return THROW_ERROR_EXCEPTION("You must provide 1 argument:ring");

	Local<Array> ring = Local<Array>::Cast(info[0]);

	uint8_t hashdata[174]; // PROOFSIZEi*EDGEBITS/8
	memset(hashdata, 0, 174);

	int bytepos = 0;
	int bitpos = 0;
	for(int i = 0; i < PROOFSIZEi; i++){

		uint32_t node = ring->Get(Nan::GetCurrentContext(), i).ToLocalChecked()->Uint32Value(Nan::GetCurrentContext()).FromJust();

		for(int j = 0; j < EDGEBITS; j++) {

			if((node >> j) & 1U)
				hashdata[bytepos] |= 1UL << bitpos;

			bitpos++;
			if(bitpos==8) {
				bitpos=0;bytepos++;
			}
		}
	}

	unsigned char cyclehash[32];
	rx_blake2b((void *)cyclehash, sizeof(cyclehash), (uint8_t *)hashdata, sizeof(hashdata));

	unsigned char rev_cyclehash[32];
	for(int i = 0; i < 32; i++)
		rev_cyclehash[i] = cyclehash[31-i];

	v8::Local<v8::Value> returnValue = Nan::CopyBuffer((char*)rev_cyclehash, 32).ToLocalChecked();
	info.GetReturnValue().Set(returnValue);
}

NAN_METHOD(kawpow) {
	if (info.Length() != 3) return THROW_ERROR_EXCEPTION("You must provide 3 argument buffers: header hash (32 bytes), nonce (8 bytes), mixhash (32 bytes)");

	v8::Isolate *isolate = v8::Isolate::GetCurrent();

	Local<Object> header_hash_buff = info[0]->ToObject(isolate->GetCurrentContext()).ToLocalChecked();
	if (!Buffer::HasInstance(header_hash_buff)) return THROW_ERROR_EXCEPTION("Argument 1 should be a buffer object.");
	if (Buffer::Length(header_hash_buff) != 32) return THROW_ERROR_EXCEPTION("Argument 1 should be a 32 bytes long buffer object.");

	Local<Object> nonce_buff = info[1]->ToObject(isolate->GetCurrentContext()).ToLocalChecked();
	if (!Buffer::HasInstance(nonce_buff)) return THROW_ERROR_EXCEPTION("Argument 2 should be a buffer object.");
	if (Buffer::Length(nonce_buff) != 8) return THROW_ERROR_EXCEPTION("Argument 2 should be a 8 bytes long buffer object.");

	Local<Object> mix_hash_buff = info[2]->ToObject(isolate->GetCurrentContext()).ToLocalChecked();
	if (!Buffer::HasInstance(mix_hash_buff)) return THROW_ERROR_EXCEPTION("Argument 3 should be a buffer object.");
	if (Buffer::Length(mix_hash_buff) != 32) return THROW_ERROR_EXCEPTION("Argument 3 should be a 8 bytes long buffer object.");

	uint32_t header_hash[8];
	memcpy(header_hash, reinterpret_cast<const uint8_t*>(Buffer::Data(header_hash_buff)), sizeof(header_hash));
        const uint64_t nonce = __builtin_bswap64(*(reinterpret_cast<const uint64_t*>(Buffer::Data(nonce_buff))));
        uint32_t mix_hash[8];
	memcpy(mix_hash, reinterpret_cast<const uint8_t*>(Buffer::Data(mix_hash_buff)), sizeof(mix_hash));

        uint32_t output[8];
	xmrig::KPHash::verify(header_hash, nonce, mix_hash, output);

	v8::Local<v8::Value> returnValue = Nan::CopyBuffer((char*)output, 32).ToLocalChecked();
	info.GetReturnValue().Set(returnValue);
}

NAN_METHOD(ethash) {
	if (info.Length() != 3) return THROW_ERROR_EXCEPTION("You must provide 3 arguments: header hash (32 bytes), nonce (8 bytes), height (integer)");

	v8::Isolate *isolate = v8::Isolate::GetCurrent();

	Local<Object> header_hash_buff = info[0]->ToObject(isolate->GetCurrentContext()).ToLocalChecked();
	if (!Buffer::HasInstance(header_hash_buff)) return THROW_ERROR_EXCEPTION("Argument 1 should be a buffer object.");
	if (Buffer::Length(header_hash_buff) != 32) return THROW_ERROR_EXCEPTION("Argument 1 should be a 32 bytes long buffer object.");

	Local<Object> nonce_buff = info[1]->ToObject(isolate->GetCurrentContext()).ToLocalChecked();
	if (!Buffer::HasInstance(nonce_buff)) return THROW_ERROR_EXCEPTION("Argument 2 should be a buffer object.");
	if (Buffer::Length(nonce_buff) != 8) return THROW_ERROR_EXCEPTION("Argument 2 should be a 8 bytes long buffer object.");

        if (!info[2]->IsNumber()) return THROW_ERROR_EXCEPTION("Argument 3 should be a number");
        const int height = Nan::To<int>(info[2]).FromMaybe(0);

	ethash_h256_t header_hash;
	memcpy(&header_hash, reinterpret_cast<const uint8_t*>(Buffer::Data(header_hash_buff)), sizeof(header_hash));
        const uint64_t nonce = __builtin_bswap64(*(reinterpret_cast<const uint64_t*>(Buffer::Data(nonce_buff))));

        static int prev_epoch = 0;
        static ethash_light_t cache = nullptr;
        const int epoch = height / ETHASH_EPOCH_LENGTH;
        if (prev_epoch != epoch) {
            if (cache) ethash_light_delete(cache);
            cache = ethash_light_new(height, epoch, epoch);
            prev_epoch = epoch;
        }
        ethash_return_value_t res = ethash_light_compute(cache, header_hash, nonce);

        v8::Local<v8::Array> returnValue = New<v8::Array>(2);
        Nan::Set(returnValue, 0, Nan::CopyBuffer((char*)&res.result.b[0], 32).ToLocalChecked());
        Nan::Set(returnValue, 1, Nan::CopyBuffer((char*)&res.mix_hash.b[0], 32).ToLocalChecked());
	info.GetReturnValue().Set(returnValue);
}

NAN_METHOD(etchash) {
	if (info.Length() != 3) return THROW_ERROR_EXCEPTION("You must provide 3 arguments: header hash (32 bytes), nonce (8 bytes), height (integer)");

	v8::Isolate *isolate = v8::Isolate::GetCurrent();

	Local<Object> header_hash_buff = info[0]->ToObject(isolate->GetCurrentContext()).ToLocalChecked();
	if (!Buffer::HasInstance(header_hash_buff)) return THROW_ERROR_EXCEPTION("Argument 1 should be a buffer object.");
	if (Buffer::Length(header_hash_buff) != 32) return THROW_ERROR_EXCEPTION("Argument 1 should be a 32 bytes long buffer object.");

	Local<Object> nonce_buff = info[1]->ToObject(isolate->GetCurrentContext()).ToLocalChecked();
	if (!Buffer::HasInstance(nonce_buff)) return THROW_ERROR_EXCEPTION("Argument 2 should be a buffer object.");
	if (Buffer::Length(nonce_buff) != 8) return THROW_ERROR_EXCEPTION("Argument 2 should be a 8 bytes long buffer object.");

        if (!info[2]->IsNumber()) return THROW_ERROR_EXCEPTION("Argument 3 should be a number");
        const int height = Nan::To<int>(info[2]).FromMaybe(0);

	ethash_h256_t header_hash;
	memcpy(&header_hash, reinterpret_cast<const uint8_t*>(Buffer::Data(header_hash_buff)), sizeof(header_hash));
        const uint64_t nonce = __builtin_bswap64(*(reinterpret_cast<const uint64_t*>(Buffer::Data(nonce_buff))));

        static int prev_epoch_seed = 0;
        static ethash_light_t cache = nullptr;
        const int epoch_length = height >= ETCHASH_EPOCH_HEIGHT ? ETCHASH_EPOCH_LENGTH : ETHASH_EPOCH_LENGTH;
        const int epoch       = height / epoch_length;
        const int epoch_seed  = (epoch * epoch_length + 1) / ETHASH_EPOCH_LENGTH;
        if (prev_epoch_seed != epoch_seed) {
            if (cache) ethash_light_delete(cache);
            cache = ethash_light_new(height, epoch_seed, epoch);
            prev_epoch_seed = epoch_seed;
        }
        ethash_return_value_t res = ethash_light_compute(cache, header_hash, nonce);

        v8::Local<v8::Array> returnValue = New<v8::Array>(2);
        Nan::Set(returnValue, 0, Nan::CopyBuffer((char*)&res.result.b[0], 32).ToLocalChecked());
        Nan::Set(returnValue, 1, Nan::CopyBuffer((char*)&res.mix_hash.b[0], 32).ToLocalChecked());
	info.GetReturnValue().Set(returnValue);
}


NAN_MODULE_INIT(init) {
    Nan::Set(target, Nan::New("cryptonight").ToLocalChecked(), Nan::GetFunction(Nan::New<FunctionTemplate>(cryptonight)).ToLocalChecked());
    Nan::Set(target, Nan::New("cryptonight_light").ToLocalChecked(), Nan::GetFunction(Nan::New<FunctionTemplate>(cryptonight_light)).ToLocalChecked());
    Nan::Set(target, Nan::New("cryptonight_plex").ToLocalChecked(), Nan::GetFunction(Nan::New<FunctionTemplate>(cryptonight_plex)).ToLocalChecked());
    Nan::Set(target, Nan::New("cryptonight_heavy").ToLocalChecked(), Nan::GetFunction(Nan::New<FunctionTemplate>(cryptonight_heavy)).ToLocalChecked());
    Nan::Set(target, Nan::New("cryptonight_pico").ToLocalChecked(), Nan::GetFunction(Nan::New<FunctionTemplate>(cryptonight_pico)).ToLocalChecked());
    Nan::Set(target, Nan::New("randomx").ToLocalChecked(), Nan::GetFunction(Nan::New<FunctionTemplate>(randomx)).ToLocalChecked());
    Nan::Set(target, Nan::New("argon2").ToLocalChecked(), Nan::GetFunction(Nan::New<FunctionTemplate>(argon2)).ToLocalChecked());
    Nan::Set(target, Nan::New("astrobwt").ToLocalChecked(), Nan::GetFunction(Nan::New<FunctionTemplate>(astrobwt)).ToLocalChecked());
    Nan::Set(target, Nan::New("k12").ToLocalChecked(), Nan::GetFunction(Nan::New<FunctionTemplate>(k12)).ToLocalChecked());
    Nan::Set(target, Nan::New("c29s").ToLocalChecked(), Nan::GetFunction(Nan::New<FunctionTemplate>(c29s)).ToLocalChecked());
    Nan::Set(target, Nan::New("c29v").ToLocalChecked(), Nan::GetFunction(Nan::New<FunctionTemplate>(c29v)).ToLocalChecked());
    Nan::Set(target, Nan::New("c29b").ToLocalChecked(), Nan::GetFunction(Nan::New<FunctionTemplate>(c29b)).ToLocalChecked());
    Nan::Set(target, Nan::New("c29i").ToLocalChecked(), Nan::GetFunction(Nan::New<FunctionTemplate>(c29i)).ToLocalChecked());
    Nan::Set(target, Nan::New("c29_cycle_hash").ToLocalChecked(), Nan::GetFunction(Nan::New<FunctionTemplate>(c29_cycle_hash)).ToLocalChecked());
    Nan::Set(target, Nan::New("c29b_cycle_hash").ToLocalChecked(), Nan::GetFunction(Nan::New<FunctionTemplate>(c29b_cycle_hash)).ToLocalChecked());
    Nan::Set(target, Nan::New("c29i_cycle_hash").ToLocalChecked(), Nan::GetFunction(Nan::New<FunctionTemplate>(c29i_cycle_hash)).ToLocalChecked());
    Nan::Set(target, Nan::New("kawpow").ToLocalChecked(), Nan::GetFunction(Nan::New<FunctionTemplate>(kawpow)).ToLocalChecked());
    Nan::Set(target, Nan::New("ethash").ToLocalChecked(), Nan::GetFunction(Nan::New<FunctionTemplate>(ethash)).ToLocalChecked());
    Nan::Set(target, Nan::New("etchash").ToLocalChecked(), Nan::GetFunction(Nan::New<FunctionTemplate>(etchash)).ToLocalChecked());
}

NODE_MODULE(cryptonight, init)
