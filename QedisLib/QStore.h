#ifndef BERT_QSTORE_H
#define BERT_QSTORE_H

#include "QCommon.h"
#include "QSet.h"
#include "QSortedSet.h"
#include "QHash.h"
#include "QList.h"
#include "Timer.h"

#include <vector>
#include <map>
#include <memory>

namespace qedis
{

typedef std::shared_ptr<QString>      PSTRING;
typedef std::shared_ptr<QList>        PLIST;
typedef std::shared_ptr<QSet>         PSET;
typedef std::shared_ptr<QSortedSet>   PSSET;
typedef std::shared_ptr<QHash>        PHASH;

struct  QObject
{
    unsigned int type : 4;
    unsigned int nouse: 2;
    unsigned int encoding: 4;
    unsigned int lru  : 22;

    std::shared_ptr<void>  value;
    
    explicit QObject(QType  t = QType_invalid) : type(t)
    {
        switch (type)
        {
            case QType_list:
                encoding = QEncode_list;
                break;
                
            case QType_hash:
                encoding = QEncode_hash;
                break;
                
            case QType_set:
                encoding = QEncode_set;
                break;

            case QType_sortedSet:
                encoding = QEncode_sset;
                break;
                
            default:
                encoding = QEncode_invalid;
                break;
        }
        
        nouse = 0;
        lru   = 0;
    }
    
    QObject(const QObject& obj) = default;
    QObject& operator= (const QObject& obj) =  default;

    QObject(QObject&& obj)
    {
        type = obj.type;
        nouse = obj.nouse;
        encoding = obj.encoding;
        lru = obj.lru;
        
        value = std::move(obj.value);
    }
    
    PSTRING  CastString()       const { return std::static_pointer_cast<QString>(value); }
    PLIST    CastList()         const { return std::static_pointer_cast<QList>(value);   }
    PSET     CastSet()          const { return std::static_pointer_cast<QSet>(value);    }
    PSSET    CastSortedSet()    const { return std::static_pointer_cast<QSortedSet>(value); }
    PHASH    CastHash()         const { return std::static_pointer_cast<QHash>(value);   }
};

class QClient;

typedef std::unordered_map<QString, QObject,
        my_hash,
        std::equal_to<QString> >  QDB;



class ExpireTimer  : public Timer
{
public:
    ExpireTimer(int db) : Timer(1)
    {
        dbno_ = db;
    }
private:
    int  dbno_;
    bool _OnTimer();
};

class BlockedListTimer  : public Timer
{
public:
    BlockedListTimer(int db) : Timer(3)
    {
        dbno_ = db;
    }
private:
    int  dbno_;
    bool _OnTimer();
};

const int kMaxDbNum = 65536;

class QStore
{
public:
    static QStore& Instance();
    
    void  Init(int dbNum = 16);

    int SelectDB(int dbno);
    int GetDB() const;
    
    // Key operation
    bool DeleteKey(const QString& key);
    bool ExistsKey(const QString& key) const;
    QType  KeyType(const QString& key) const;
    QString RandomKey() const;
    size_t DBSize() const { return store_[dbno_].size(); }
    size_t ScanKey(size_t cursor, size_t count, std::vector<QString>& res) const;

    // iterator
    QDB::const_iterator begin() const   { return store_[dbno_].begin(); }
    QDB::const_iterator end()   const   { return store_[dbno_].end(); }
    QDB::iterator       begin()         { return store_[dbno_].begin(); }
    QDB::iterator       end()           { return store_[dbno_].end(); }
    
    QError  GetValue(const QString& key, QObject*& value);
    QError  GetValueByType(const QString& key, QObject*& value, QType  type = QType_invalid);
    QObject*SetValue(const QString& key, const QObject& value);
    bool    SetValueIfNotExist(const QString& key, const QObject& value);

    // for expire key
    enum ExpireResult : std::int8_t
    {
        notExpire=  0,
        persist  = -1,
        expired  = -2,
        notExist = -2,
    };
    void    SetExpire(const QString& key, uint64_t when);
    int64_t TTL(const QString& key, uint64_t now);
    bool    ClearExpire(const QString& key);
    int     LoopCheckExpire(uint64_t now);
    void    InitExpireTimer();
    
    // danger cmd
    void    ClearCurrentDB() { store_[dbno_].clear(); }
    void    ResetDb();
    
    // for blocked list
    bool    BlockClient(const QString& key,
                        QClient* client,
                        uint64_t timeout,
                        ListPosition pos,
                        const QString* dstList = 0);
    size_t  UnblockClient(QClient* client);
    size_t  ServeClient(const QString& key, const PLIST& list);
    
    int     LoopCheckBlocked(uint64_t now);
    void    InitBlockedTimer();
    
    size_t  BlockedSize() const;
    
    static  int dirty_;
    
    // password for auth
    QString password_;
    bool    CheckPassword(const QString& pwd) const { return password_.empty() || password_ == pwd; }
    
private:
    QStore() : dbno_(0)
    {
    }
    
    ExpireResult    _ExpireIfNeed(const QString& key, uint64_t now);
    
    class ExpiresDB
    {
    public:
        void    SetExpire(const QString& key, uint64_t when);
        int64_t TTL(const QString& key, uint64_t now);
        bool    ClearExpire(const QString& key);
        ExpireResult    ExpireIfNeed(const QString& key, uint64_t now);

        int     LoopCheck(uint64_t now);
        
    private:
        typedef std::unordered_map<QString, uint64_t,
                                    my_hash,
                                    std::equal_to<QString> >
                               Q_EXPIRE_DB;
        Q_EXPIRE_DB            expireKeys_;  // all the keys to be expired, unorder.
    };
    
    class BlockedClients
    {
    public:
        bool    BlockClient(const QString& key,
                            QClient* client,
                            uint64_t timeout,
                            ListPosition  pos,
                            const QString* dstList = 0);
        size_t  UnblockClient(QClient* client);
        size_t  ServeClient(const QString& key, const PLIST& list);
        
        int    LoopCheck(uint64_t now);
        size_t Size() const { return blockedClients_.size(); }
    private:
        typedef std::list<std::tuple<std::weak_ptr<QClient>, uint64_t, ListPosition> >   Clients;
        typedef std::unordered_map<QString, Clients>  WaitingList;
        
        WaitingList  blockedClients_;
    };

    QError  _SetValue(const QString& key, QObject& value, bool exclusive = false);

    std::vector<QDB>  store_;
    std::vector<ExpiresDB> expiresDb_;
    std::vector<BlockedClients> blockedClients_;
    int               dbno_;
};

#define QSTORE  QStore::Instance()
#define QEDIS_VERSION "0.7.0"

// ugly, but I don't want to write signalModifiedKey() every where
extern std::vector<QString> g_dirtyKeys;
extern void Propogate(const std::vector<QString>& params);
    
}

#endif

