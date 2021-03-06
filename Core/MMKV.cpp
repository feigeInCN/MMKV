/*
 * Tencent is pleased to support the open source community by making
 * MMKV available.
 *
 * Copyright (C) 2018 THL A29 Limited, a Tencent company.
 * All rights reserved.
 *
 * Licensed under the BSD 3-Clause License (the "License"); you may not use
 * this file except in compliance with the License. You may obtain a copy of
 * the License at
 *
 *       https://opensource.org/licenses/BSD-3-Clause
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "CodedInputData.h"
#include "CodedOutputData.h"
#include "InterProcessLock.h"
#include "KeyValueHolder.h"
#include "MMBuffer.h"
#include "MMKVLog.h"
#include "MMKVMetaInfo.hpp"
#include "MMKV_IO.h"
#include "MemoryFile.h"
#include "MiniPBCoder.h"
#include "PBUtility.h"
#include "ScopedLock.hpp"
#include "ThreadLock.h"
#include "aes/AESCrypt.h"
#include "aes/openssl/openssl_aes.h"
#include "aes/openssl/openssl_md5.h"
#include "crc32/Checksum.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

#ifdef MMKV_APPLE
#    if __has_feature(objc_arc)
#        error This file must be compiled with MRC. Use -fno-objc-arc flag.
#    endif
#endif // MMKV_APPLE

using namespace std;
using namespace mmkv;

unordered_map<std::string, MMKV *> *g_instanceDic;
ThreadLock *g_instanceLock;
MMKVPath_t g_rootDir;
static mmkv::ErrorHandler g_errorHandler;
size_t mmkv::DEFAULT_MMAP_SIZE;

#ifndef MMKV_WIN32
constexpr auto SPECIAL_CHARACTER_DIRECTORY_NAME = "specialCharacter";
#else
constexpr auto SPECIAL_CHARACTER_DIRECTORY_NAME = L"specialCharacter";
#endif
constexpr uint32_t Fixed32Size = pbFixed32Size();

MMKV_NAMESPACE_BEGIN

#ifndef MMKV_ANDROID
MMKV::MMKV(const std::string &mmapID, MMKVMode mode, string *cryptKey, MMKVPath_t *relativePath)
    : m_mmapID(mmapID)
    , m_path(mappedKVPathWithID(m_mmapID, mode, relativePath))
    , m_crcPath(crcPathWithID(m_mmapID, mode, relativePath))
    , m_dic(nullptr)
    , m_dicCrypt(nullptr)
    , m_file(new MemoryFile(m_path))
    , m_metaFile(new MemoryFile(m_crcPath))
    , m_metaInfo(new MMKVMetaInfo())
    , m_crypter(nullptr)
    , m_lock(new ThreadLock())
    , m_fileLock(new FileLock(m_metaFile->getFd()))
    , m_sharedProcessLock(new InterProcessLock(m_fileLock, SharedLockType))
    , m_exclusiveProcessLock(new InterProcessLock(m_fileLock, ExclusiveLockType))
    , m_isInterProcess((mode & MMKV_MULTI_PROCESS) != 0) {
    m_actualSize = 0;
    m_output = nullptr;

    if (cryptKey && cryptKey->length() > 0) {
        m_dicCrypt = new MMKVMapCrypt();
        m_crypter = new AESCrypt(cryptKey->data(), cryptKey->length());
    } else {
        m_dic = new MMKVMap();
    }

    m_needLoadFromFile = true;
    m_hasFullWriteback = false;

    m_crcDigest = 0;

    m_lock->initialize();
    m_sharedProcessLock->m_enable = m_isInterProcess;
    m_exclusiveProcessLock->m_enable = m_isInterProcess;

    // sensitive zone
    {
        SCOPED_LOCK(m_sharedProcessLock);
        loadFromFile();
    }
}
#endif

MMKV::~MMKV() {
    clearMemoryCache();

    delete m_dic;
    delete m_dicCrypt;
    delete m_crypter;
    delete m_file;
    delete m_metaFile;
    delete m_metaInfo;
    delete m_lock;
    delete m_fileLock;
    delete m_sharedProcessLock;
    delete m_exclusiveProcessLock;
}

MMKV *MMKV::defaultMMKV(MMKVMode mode, string *cryptKey) {
#ifndef MMKV_ANDROID
    return mmkvWithID(DEFAULT_MMAP_ID, mode, cryptKey);
#else
    return mmkvWithID(DEFAULT_MMAP_ID, DEFAULT_MMAP_SIZE, mode, cryptKey);
#endif
}

void initialize() {
    g_instanceDic = new unordered_map<string, MMKV *>;
    g_instanceLock = new ThreadLock();
    g_instanceLock->initialize();

    mmkv::DEFAULT_MMAP_SIZE = mmkv::getPageSize();
    MMKVInfo("version %s page size:%d", MMKV_VERSION, DEFAULT_MMAP_SIZE);
#ifndef NDEBUG
    AESCrypt::testAESCrypt();
    KeyValueHolderCrypt::testAESToMMBuffer();
#endif
}

ThreadOnceToken_t once_control = ThreadOnceUninitialized;

void MMKV::initializeMMKV(const MMKVPath_t &rootDir, MMKVLogLevel logLevel) {
    g_currentLogLevel = logLevel;

    ThreadLock::ThreadOnce(&once_control, initialize);

    g_rootDir = rootDir;
    mkPath(g_rootDir);

    MMKVInfo("root dir: " MMKV_PATH_FORMAT, g_rootDir.c_str());
}

#ifndef MMKV_ANDROID
MMKV *MMKV::mmkvWithID(const string &mmapID, MMKVMode mode, string *cryptKey, MMKVPath_t *relativePath) {

    if (mmapID.empty()) {
        return nullptr;
    }
    SCOPED_LOCK(g_instanceLock);

    auto mmapKey = mmapedKVKey(mmapID, relativePath);
    auto itr = g_instanceDic->find(mmapKey);
    if (itr != g_instanceDic->end()) {
        MMKV *kv = itr->second;
        return kv;
    }

    if (relativePath) {
        MMKVPath_t specialPath = (*relativePath) + MMKV_PATH_SLASH + SPECIAL_CHARACTER_DIRECTORY_NAME;
        if (!isFileExist(specialPath)) {
            mkPath(specialPath);
        }
        MMKVInfo("prepare to load %s (id %s) from relativePath %s", mmapID.c_str(), mmapKey.c_str(),
                 relativePath->c_str());
    }

    auto kv = new MMKV(mmapID, mode, cryptKey, relativePath);
    kv->m_mmapKey = mmapKey;
    (*g_instanceDic)[mmapKey] = kv;
    return kv;
}
#endif

void MMKV::onExit() {
    SCOPED_LOCK(g_instanceLock);

    for (auto &pair : *g_instanceDic) {
        MMKV *kv = pair.second;
        kv->sync();
        kv->clearMemoryCache();
        delete kv;
        pair.second = nullptr;
    }

    delete g_instanceDic;
    g_instanceDic = nullptr;
}

const string &MMKV::mmapID() {
    return m_mmapID;
}

string MMKV::cryptKey() {
    SCOPED_LOCK(m_lock);

    if (m_crypter) {
        char key[AES_KEY_LEN];
        m_crypter->getKey(key);
        return string(key, strnlen(key, AES_KEY_LEN));
    }
    return "";
}

mmkv::ContentChangeHandler g_contentChangeHandler = nullptr;

void MMKV::notifyContentChanged() {
    if (g_contentChangeHandler) {
        g_contentChangeHandler(m_mmapID);
    }
}

void MMKV::checkContentChanged() {
    SCOPED_LOCK(m_lock);
    checkLoadData();
}

void MMKV::registerContentChangeHandler(mmkv::ContentChangeHandler handler) {
    g_contentChangeHandler = handler;
}

void MMKV::unRegisterContentChangeHandler() {
    g_contentChangeHandler = nullptr;
}

void MMKV::clearMemoryCache() {
    MMKVInfo("clearMemoryCache [%s]", m_mmapID.c_str());
    SCOPED_LOCK(m_lock);
    if (m_needLoadFromFile) {
        return;
    }
    m_needLoadFromFile = true;

    clearDictionary(m_dicCrypt);
    clearDictionary(m_dic);

    m_hasFullWriteback = false;

    if (m_crypter) {
        if (m_metaInfo->m_version >= MMKVVersionRandomIV) {
            m_crypter->resetIV(m_metaInfo->m_vector, sizeof(m_metaInfo->m_vector));
        } else {
            m_crypter->resetIV();
        }
    }

    delete m_output;
    m_output = nullptr;

    m_file->clearMemoryCache();
    m_actualSize = 0;
    m_metaInfo->m_crcDigest = 0;
}

void MMKV::close() {
    MMKVInfo("close [%s]", m_mmapID.c_str());
    SCOPED_LOCK(g_instanceLock);
    m_lock->lock();

#ifndef MMKV_ANDROID
    auto itr = g_instanceDic->find(m_mmapKey);
#else
    auto itr = g_instanceDic->find(m_mmapID);
#endif
    if (itr != g_instanceDic->end()) {
        g_instanceDic->erase(itr);
    }
    delete this;
}

void MMKV::checkReSetCryptKey(const string *cryptKey) {
    SCOPED_LOCK(m_lock);

    if (m_crypter) {
        if (cryptKey && cryptKey->length() > 0) {
            string oldKey = this->cryptKey();
            if (oldKey != *cryptKey) {
                MMKVInfo("setting new aes key");
                delete m_crypter;
                auto ptr = cryptKey->data();
                m_crypter = new AESCrypt(ptr, cryptKey->length());

                checkLoadData();
            } else {
                // nothing to do
            }
        } else {
            MMKVInfo("reset aes key");
            delete m_crypter;
            m_crypter = nullptr;

            checkLoadData();
        }
    } else {
        if (cryptKey && cryptKey->length() > 0) {
            MMKVInfo("setting new aes key");
            auto ptr = cryptKey->data();
            m_crypter = new AESCrypt(ptr, cryptKey->length());

            checkLoadData();
        } else {
            // nothing to do
        }
    }
}

bool MMKV::isFileValid() {
    return m_file->isFileValid();
}

// crc

// assuming m_file is valid
bool MMKV::checkFileCRCValid(size_t actualSize, uint32_t crcDigest) {
    auto ptr = (uint8_t *) m_file->getMemory();
    if (ptr) {
        m_crcDigest = (uint32_t) CRC32(0, (const uint8_t *) ptr + Fixed32Size, (uint32_t) actualSize);

        if (m_crcDigest == crcDigest) {
            return true;
        }
        MMKVError("check crc [%s] fail, crc32:%u, m_crcDigest:%u", m_mmapID.c_str(), crcDigest, m_crcDigest);
    }
    return false;
}

void MMKV::recaculateCRCDigestWithIV(const void *iv) {
    auto ptr = (const uint8_t *) m_file->getMemory();
    if (ptr) {
        m_crcDigest = 0;
        m_crcDigest = (uint32_t) CRC32(0, ptr + Fixed32Size, (uint32_t) m_actualSize);
        writeActualSize(m_actualSize, m_crcDigest, iv, IncreaseSequence);
    }
}

void MMKV::updateCRCDigest(const uint8_t *ptr, size_t length) {
    if (ptr == nullptr) {
        return;
    }
    m_crcDigest = (uint32_t) CRC32(m_crcDigest, ptr, (uint32_t) length);

    writeActualSize(m_actualSize, m_crcDigest, nullptr, KeepSequence);
}

// set & get

bool MMKV::set(bool value, MMKVKey_t key) {
    if (isKeyEmpty(key)) {
        return false;
    }
    size_t size = pbBoolSize();
    MMBuffer data(size);
    CodedOutputData output(data.getPtr(), size);
    output.writeBool(value);

    return setDataForKey(move(data), key);
}

bool MMKV::set(int32_t value, MMKVKey_t key) {
    if (isKeyEmpty(key)) {
        return false;
    }
    size_t size = pbInt32Size(value);
    MMBuffer data(size);
    CodedOutputData output(data.getPtr(), size);
    output.writeInt32(value);

    return setDataForKey(move(data), key);
}

bool MMKV::set(uint32_t value, MMKVKey_t key) {
    if (isKeyEmpty(key)) {
        return false;
    }
    size_t size = pbUInt32Size(value);
    MMBuffer data(size);
    CodedOutputData output(data.getPtr(), size);
    output.writeUInt32(value);

    return setDataForKey(move(data), key);
}

bool MMKV::set(int64_t value, MMKVKey_t key) {
    if (isKeyEmpty(key)) {
        return false;
    }
    size_t size = pbInt64Size(value);
    MMBuffer data(size);
    CodedOutputData output(data.getPtr(), size);
    output.writeInt64(value);

    return setDataForKey(move(data), key);
}

bool MMKV::set(uint64_t value, MMKVKey_t key) {
    if (isKeyEmpty(key)) {
        return false;
    }
    size_t size = pbUInt64Size(value);
    MMBuffer data(size);
    CodedOutputData output(data.getPtr(), size);
    output.writeUInt64(value);

    return setDataForKey(move(data), key);
}

bool MMKV::set(float value, MMKVKey_t key) {
    if (isKeyEmpty(key)) {
        return false;
    }
    size_t size = pbFloatSize();
    MMBuffer data(size);
    CodedOutputData output(data.getPtr(), size);
    output.writeFloat(value);

    return setDataForKey(move(data), key);
}

bool MMKV::set(double value, MMKVKey_t key) {
    if (isKeyEmpty(key)) {
        return false;
    }
    size_t size = pbDoubleSize();
    MMBuffer data(size);
    CodedOutputData output(data.getPtr(), size);
    output.writeDouble(value);

    return setDataForKey(move(data), key);
}

#ifndef MMKV_APPLE

bool MMKV::set(const char *value, MMKVKey_t key) {
    if (!value) {
        removeValueForKey(key);
        return true;
    }
    return setDataForKey(MMBuffer((void *) value, strlen(value), MMBufferNoCopy), key, true);
}

bool MMKV::set(const string &value, MMKVKey_t key) {
    if (isKeyEmpty(key)) {
        return false;
    }
    return setDataForKey(MMBuffer((void *) value.data(), value.length(), MMBufferNoCopy), key, true);
}

bool MMKV::set(const MMBuffer &value, MMKVKey_t key) {
    if (isKeyEmpty(key)) {
        return false;
    }
    // delay write the size needed for encoding value
    // avoid memory copying
    return setDataForKey(MMBuffer(value.getPtr(), value.length(), MMBufferNoCopy), key, true);
}

bool MMKV::set(const vector<string> &v, MMKVKey_t key) {
    if (isKeyEmpty(key)) {
        return false;
    }
    auto data = MiniPBCoder::encodeDataWithObject(v);
    return setDataForKey(move(data), key);
}

bool MMKV::getString(MMKVKey_t key, string &result) {
    if (isKeyEmpty(key)) {
        return false;
    }
    SCOPED_LOCK(m_lock);
    auto data = getDataForKey(key);
    if (data.length() > 0) {
        try {
            CodedInputData input(data.getPtr(), data.length());
            result = input.readString();
            return true;
        } catch (std::exception &exception) {
            MMKVError("%s", exception.what());
        }
    }
    return false;
}

MMBuffer MMKV::getBytes(MMKVKey_t key) {
    if (isKeyEmpty(key)) {
        return MMBuffer();
    }
    SCOPED_LOCK(m_lock);
    auto data = getDataForKey(key);
    if (data.length() > 0) {
        try {
            CodedInputData input(data.getPtr(), data.length());
            return input.readData();
        } catch (std::exception &exception) {
            MMKVError("%s", exception.what());
        }
    }
    return MMBuffer();
}

bool MMKV::getVector(MMKVKey_t key, vector<string> &result) {
    if (isKeyEmpty(key)) {
        return false;
    }
    SCOPED_LOCK(m_lock);
    auto data = getDataForKey(key);
    if (data.length() > 0) {
        try {
            result = MiniPBCoder::decodeVector(data);
            return true;
        } catch (std::exception &exception) {
            MMKVError("%s", exception.what());
        }
    }
    return false;
}

#endif // MMKV_APPLE

bool MMKV::getBool(MMKVKey_t key, bool defaultValue) {
    if (isKeyEmpty(key)) {
        return defaultValue;
    }
    SCOPED_LOCK(m_lock);
    auto data = getDataForKey(key);
    if (data.length() > 0) {
        try {
            CodedInputData input(data.getPtr(), data.length());
            return input.readBool();
        } catch (std::exception &exception) {
            MMKVError("%s", exception.what());
        }
    }
    return defaultValue;
}

int32_t MMKV::getInt32(MMKVKey_t key, int32_t defaultValue) {
    if (isKeyEmpty(key)) {
        return defaultValue;
    }
    SCOPED_LOCK(m_lock);
    auto data = getDataForKey(key);
    if (data.length() > 0) {
        try {
            CodedInputData input(data.getPtr(), data.length());
            return input.readInt32();
        } catch (std::exception &exception) {
            MMKVError("%s", exception.what());
        }
    }
    return defaultValue;
}

uint32_t MMKV::getUInt32(MMKVKey_t key, uint32_t defaultValue) {
    if (isKeyEmpty(key)) {
        return defaultValue;
    }
    SCOPED_LOCK(m_lock);
    auto data = getDataForKey(key);
    if (data.length() > 0) {
        try {
            CodedInputData input(data.getPtr(), data.length());
            return input.readUInt32();
        } catch (std::exception &exception) {
            MMKVError("%s", exception.what());
        }
    }
    return defaultValue;
}

int64_t MMKV::getInt64(MMKVKey_t key, int64_t defaultValue) {
    if (isKeyEmpty(key)) {
        return defaultValue;
    }
    SCOPED_LOCK(m_lock);
    auto data = getDataForKey(key);
    if (data.length() > 0) {
        try {
            CodedInputData input(data.getPtr(), data.length());
            return input.readInt64();
        } catch (std::exception &exception) {
            MMKVError("%s", exception.what());
        }
    }
    return defaultValue;
}

uint64_t MMKV::getUInt64(MMKVKey_t key, uint64_t defaultValue) {
    if (isKeyEmpty(key)) {
        return defaultValue;
    }
    SCOPED_LOCK(m_lock);
    auto data = getDataForKey(key);
    if (data.length() > 0) {
        try {
            CodedInputData input(data.getPtr(), data.length());
            return input.readUInt64();
        } catch (std::exception &exception) {
            MMKVError("%s", exception.what());
        }
    }
    return defaultValue;
}

float MMKV::getFloat(MMKVKey_t key, float defaultValue) {
    if (isKeyEmpty(key)) {
        return defaultValue;
    }
    SCOPED_LOCK(m_lock);
    auto data = getDataForKey(key);
    if (data.length() > 0) {
        try {
            CodedInputData input(data.getPtr(), data.length());
            return input.readFloat();
        } catch (std::exception &exception) {
            MMKVError("%s", exception.what());
        }
    }
    return defaultValue;
}

double MMKV::getDouble(MMKVKey_t key, double defaultValue) {
    if (isKeyEmpty(key)) {
        return defaultValue;
    }
    SCOPED_LOCK(m_lock);
    auto data = getDataForKey(key);
    if (data.length() > 0) {
        try {
            CodedInputData input(data.getPtr(), data.length());
            return input.readDouble();
        } catch (std::exception &exception) {
            MMKVError("%s", exception.what());
        }
    }
    return defaultValue;
}

size_t MMKV::getValueSize(MMKVKey_t key, bool actualSize) {
    if (isKeyEmpty(key)) {
        return 0;
    }
    SCOPED_LOCK(m_lock);
    auto data = getDataForKey(key);
    if (actualSize) {
        try {
            CodedInputData input(data.getPtr(), data.length());
            auto length = input.readInt32();
            if (length >= 0) {
                auto s_length = static_cast<size_t>(length);
                if (pbRawVarint32Size(length) + s_length == data.length()) {
                    return s_length;
                }
            }
        } catch (std::exception &exception) {
            MMKVError("%s", exception.what());
        }
    }
    return data.length();
}

int32_t MMKV::writeValueToBuffer(MMKVKey_t key, void *ptr, int32_t size) {
    if (isKeyEmpty(key) || size < 0) {
        return -1;
    }
    auto s_size = static_cast<size_t>(size);

    SCOPED_LOCK(m_lock);
    auto data = getDataForKey(key);
    try {
        CodedInputData input(data.getPtr(), data.length());
        auto length = input.readInt32();
        auto offset = pbRawVarint32Size(length);
        if (length >= 0) {
            auto s_length = static_cast<size_t>(length);
            if (offset + s_length == data.length()) {
                if (s_length <= s_size) {
                    memcpy(ptr, (uint8_t *) data.getPtr() + offset, s_length);
                    return length;
                }
            } else {
                if (data.length() <= s_size) {
                    memcpy(ptr, data.getPtr(), data.length());
                    return static_cast<int32_t>(data.length());
                }
            }
        }
    } catch (std::exception &exception) {
        MMKVError("%s", exception.what());
    }
    return -1;
}

// enumerate

bool MMKV::containsKey(MMKVKey_t key) {
    SCOPED_LOCK(m_lock);
    checkLoadData();

    if (m_crypter) {
        return m_dicCrypt->find(key) != m_dicCrypt->end();
    } else {
        return m_dic->find(key) != m_dic->end();
    }
}

size_t MMKV::count() {
    SCOPED_LOCK(m_lock);
    checkLoadData();
    if (m_crypter) {
        return m_dicCrypt->size();
    } else {
        return m_dic->size();
    }
}

size_t MMKV::totalSize() {
    SCOPED_LOCK(m_lock);
    checkLoadData();
    return m_file->getFileSize();
}

size_t MMKV::actualSize() {
    SCOPED_LOCK(m_lock);
    checkLoadData();
    return m_actualSize;
}

void MMKV::removeValueForKey(MMKVKey_t key) {
    if (isKeyEmpty(key)) {
        return;
    }
    SCOPED_LOCK(m_lock);
    SCOPED_LOCK(m_exclusiveProcessLock);
    checkLoadData();

    removeDataForKey(key);
}

#ifndef MMKV_APPLE

vector<string> MMKV::allKeys() {
    SCOPED_LOCK(m_lock);
    checkLoadData();

    vector<string> keys;
    if (m_crypter) {
        for (const auto &itr : *m_dicCrypt) {
            keys.push_back(itr.first);
        }
    } else {
        for (const auto &itr : *m_dic) {
            keys.push_back(itr.first);
        }
    }
    return keys;
}

void MMKV::removeValuesForKeys(const vector<string> &arrKeys) {
    if (arrKeys.empty()) {
        return;
    }
    if (arrKeys.size() == 1) {
        return removeValueForKey(arrKeys[0]);
    }

    SCOPED_LOCK(m_lock);
    SCOPED_LOCK(m_exclusiveProcessLock);
    checkLoadData();

    size_t deleteCount = 0;
    if (m_crypter) {
        for (const auto &key : arrKeys) {
            auto itr = m_dicCrypt->find(key);
            if (itr != m_dicCrypt->end()) {
                m_dicCrypt->erase(itr);
                deleteCount++;
            }
        }
    } else {
        for (const auto &key : arrKeys) {
            auto itr = m_dic->find(key);
            if (itr != m_dic->end()) {
                m_dic->erase(itr);
                deleteCount++;
            }
        }
    }
    if (deleteCount > 0) {
        m_hasFullWriteback = false;

        fullWriteback();
    }
}

#endif // MMKV_APPLE

// file

void MMKV::sync(SyncFlag flag) {
    SCOPED_LOCK(m_lock);
    if (m_needLoadFromFile || !isFileValid()) {
        return;
    }
    SCOPED_LOCK(m_exclusiveProcessLock);

    m_file->msync(flag);
    m_metaFile->msync(flag);
}

void MMKV::lock() {
    m_exclusiveProcessLock->lock();
}
void MMKV::unlock() {
    m_exclusiveProcessLock->unlock();
}
bool MMKV::try_lock() {
    return m_exclusiveProcessLock->try_lock();
}

void MMKV::registerErrorHandler(ErrorHandler handler) {
    SCOPED_LOCK(g_instanceLock);
    g_errorHandler = handler;
}

void MMKV::unRegisterErrorHandler() {
    SCOPED_LOCK(g_instanceLock);
    g_errorHandler = nullptr;
}

void MMKV::registerLogHandler(LogHandler handler) {
    SCOPED_LOCK(g_instanceLock);
    g_logHandler = handler;
}

void MMKV::unRegisterLogHandler() {
    SCOPED_LOCK(g_instanceLock);
    g_logHandler = nullptr;
}

void MMKV::setLogLevel(MMKVLogLevel level) {
    SCOPED_LOCK(g_instanceLock);
    g_currentLogLevel = level;
}

static void mkSpecialCharacterFileDirectory() {
    MMKVPath_t path = g_rootDir + MMKV_PATH_SLASH + SPECIAL_CHARACTER_DIRECTORY_NAME;
    mkPath(path);
}

template <typename T>
static string md5(const basic_string<T> &value) {
    uint8_t md[MD5_DIGEST_LENGTH] = {};
    char tmp[3] = {}, buf[33] = {};
    openssl::MD5((const uint8_t *) value.c_str(), value.size() * (sizeof(T) / sizeof(uint8_t)), md);
    for (auto ch : md) {
        snprintf(tmp, sizeof(tmp), "%2.2x", ch);
        strcat(buf, tmp);
    }
    return string(buf);
}

static MMKVPath_t encodeFilePath(const string &mmapID) {
    const char *specialCharacters = "\\/:*?\"<>|";
    string encodedID;
    bool hasSpecialCharacter = false;
    for (auto ch : mmapID) {
        if (strchr(specialCharacters, ch) != nullptr) {
            encodedID = md5(mmapID);
            hasSpecialCharacter = true;
            break;
        }
    }
    if (hasSpecialCharacter) {
        static ThreadOnceToken_t once_control = ThreadOnceUninitialized;
        ThreadLock::ThreadOnce(&once_control, mkSpecialCharacterFileDirectory);
        return MMKVPath_t(SPECIAL_CHARACTER_DIRECTORY_NAME) + MMKV_PATH_SLASH + string2MMKVPath_t(encodedID);
    } else {
        return string2MMKVPath_t(mmapID);
    }
}

string mmapedKVKey(const string &mmapID, MMKVPath_t *relativePath) {
    if (relativePath && g_rootDir != (*relativePath)) {
        return md5(*relativePath + MMKV_PATH_SLASH + string2MMKVPath_t(mmapID));
    }
    return mmapID;
}

MMKVPath_t mappedKVPathWithID(const string &mmapID, MMKVMode mode, MMKVPath_t *relativePath) {
#ifndef MMKV_ANDROID
    if (relativePath) {
#else
    if (mode & MMKV_ASHMEM) {
        return ashmemMMKVPathWithID(encodeFilePath(mmapID));
    } else if (relativePath) {
#endif
        return *relativePath + MMKV_PATH_SLASH + encodeFilePath(mmapID);
    }
    return g_rootDir + MMKV_PATH_SLASH + encodeFilePath(mmapID);
}

#ifndef MMKV_WIN32
constexpr auto CRC_SUFFIX = ".crc";
#else
constexpr auto CRC_SUFFIX = L".crc";
#endif

MMKVPath_t crcPathWithID(const string &mmapID, MMKVMode mode, MMKVPath_t *relativePath) {
#ifndef MMKV_ANDROID
    if (relativePath) {
#else
    if (mode & MMKV_ASHMEM) {
        return ashmemMMKVPathWithID(encodeFilePath(mmapID)) + CRC_SUFFIX;
    } else if (relativePath) {
#endif
        return *relativePath + MMKV_PATH_SLASH + encodeFilePath(mmapID) + CRC_SUFFIX;
    }
    return g_rootDir + MMKV_PATH_SLASH + encodeFilePath(mmapID) + CRC_SUFFIX;
}

MMKVRecoverStrategic onMMKVCRCCheckFail(const string &mmapID) {
    if (g_errorHandler) {
        return g_errorHandler(mmapID, MMKVErrorType::MMKVCRCCheckFail);
    }
    return OnErrorDiscard;
}

MMKVRecoverStrategic onMMKVFileLengthError(const string &mmapID) {
    if (g_errorHandler) {
        return g_errorHandler(mmapID, MMKVErrorType::MMKVFileLength);
    }
    return OnErrorDiscard;
}

MMKV_NAMESPACE_END
