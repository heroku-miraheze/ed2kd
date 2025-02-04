#include "db.h"
#include <string.h>

#include "sqlite3/sqlite3.h"
#include "ed2k_proto.h"
#include "packet.h"
#include "log.h"
#include "client.h"

static uint64_t sdbm(const unsigned char *str, size_t length)
{
    uint64_t hash = 0;
    size_t i;

    for (i = 0; i < length; ++i)
        hash = (*str++) + (hash << 6) + (hash << 16) - hash;

    return hash;
}

#define DB_NAME                 "file:memdb?mode=memory&cache=shared"
#define DB_OPEN_FLAGS           SQLITE_OPEN_CREATE|SQLITE_OPEN_READWRITE|SQLITE_OPEN_NOMUTEX|SQLITE_OPEN_SHAREDCACHE|SQLITE_OPEN_URI
#define MAX_SEARCH_QUERY_LEN    1024
#define MAX_NAME_TERM_LEN       1024

#define DB_CHECK(x)         if (!(x)) goto failed;
#define MAKE_FID(x)         sdbm((x), 16)
#define MAKE_SID(x)         ( ((uint64_t)(x)->id<<32) | (uint64_t)(x)->port )
#define GET_SID_ID(sid)     (uint32_t)((sid)>>32)
#define GET_SID_PORT(sid)   (uint16_t)(sid)

enum query_statements {
    SHARE_UPD,
    SHARE_INS,
    SHARE_SRC,
    REMOVE_SRC,
    GET_SRC,
    STMT_COUNT
};

static THREAD_LOCAL sqlite3
*
s_db;
static THREAD_LOCAL sqlite3_stmt
*s_stmt[STMT_COUNT];

int db_create(void)
{
    static const char query[] =
            "PRAGMA synchronous = 0;"
                    "PRAGMA journal_mode = OFF;"

                    "CREATE TABLE IF NOT EXISTS files ("
                    "   fid INTEGER PRIMARY KEY,"
                    "	hash BLOB NOT NULL,"
                    "	name TEXT NOT NULL,"
                    "   ext TEXT,"
                    "   size INTEGER NOT NULL,"
                    "   type INTEGER NOT NULL,"
                    "   srcavail INTEGER DEFAULT 0,"
                    "   srccomplete INTEGER DEFAULT 0,"
                    "   rating INTEGER DEFAULT 0,"
                    "   rated_count INTEGER DEFAULT 0,"
                    "   mlength INTEGER,"
                    "   mbitrate INTEGER,"
                    "   mcodec TEXT"
                    ");"

                    "CREATE VIRTUAL TABLE IF NOT EXISTS fnames USING fts4 ("
                    "   content=\"files\", tokenize=unicode61, name"
                    ");"

                    "CREATE TABLE IF NOT EXISTS sources ("
                    "   fid INTEGER NOT NULL,"
                    "   sid INTEGER NOT NULL,"
                    "   complete INTEGER,"
                    "   rating INTEGER"
                    ");"
                    "CREATE INDEX IF NOT EXISTS sources_fid_i"
                    "   ON sources(fid);"
                    "CREATE INDEX IF NOT EXISTS sources_sid_i"
                    "   ON sources(sid);"

                    "CREATE TRIGGER IF NOT EXISTS sources_ai AFTER INSERT ON sources BEGIN"
                    "   UPDATE files SET srcavail=srcavail+1,srccomplete=srccomplete+new.complete,"
                    "       rating=rating+new.rating, rated_count = CASE WHEN new.rating<>0 THEN rated_count+1 ELSE 0 END"
                    "   WHERE fid=new.fid;"
                    "END;"
                    "CREATE TRIGGER IF NOT EXISTS sources_bd BEFORE DELETE ON sources BEGIN"
                    "   UPDATE files SET srcavail=srcavail-1,srccomplete=srccomplete-old.complete,"
                    "       rating=rating-old.rating, rated_count = CASE WHEN old.rating<>0 THEN rated_count-1 ELSE rated_count END"
                    "   WHERE fid=old.fid;"
                    "END;"

                    // delete when no sources available
                    " CREATE TRIGGER IF NOT EXISTS files_au AFTER UPDATE ON files WHEN new.srcavail=0 BEGIN"
                    "   DELETE FROM files WHERE fid=new.fid;"
                    "END;"

                    // update on file name change
                    "CREATE TRIGGER IF NOT EXISTS files_fts1 BEFORE UPDATE ON files WHEN new.name<>old.name BEGIN"
                    "   DELETE FROM fnames WHERE docid=old.rowid;"
                    "END;"
                    "CREATE TRIGGER IF NOT EXISTS files_fts2 AFTER UPDATE ON files WHEN new.name<>old.name BEGIN"
                    "   INSERT INTO fnames(docid, name) VALUES(new.rowid, new.name);"
                    "END;"
                    // delete
                    "CREATE TRIGGER IF NOT EXISTS files_fts3 BEFORE DELETE ON files BEGIN"
                    "   DELETE FROM fnames WHERE docid=old.rowid;"
                    "END;"
                    // insert
                    "CREATE TRIGGER IF NOT EXISTS files_fts4 AFTER INSERT ON files BEGIN"
                    "   INSERT INTO fnames(docid, name) VALUES(new.rowid, new.name);"
                    "END;"

                    "DELETE FROM files;"
                    "DELETE FROM fnames;"
                    "DELETE FROM sources;";

    int err;

    if (!sqlite3_threadsafe()) {
        ED2KD_LOGERR("sqlite3 must be threadsafe");
        return 0;
    }

    err = sqlite3_open_v2(DB_NAME, &s_db, DB_OPEN_FLAGS, NULL);

    if (SQLITE_OK == err) {
        char *errmsg;

        err = sqlite3_exec(s_db, query, NULL, NULL, &errmsg);
        if (SQLITE_OK != err) {
            ED2KD_LOGERR("failed to execute database init script (%s)", errmsg);
            sqlite3_free(errmsg);
            return 0;
        }
    } else {
        ED2KD_LOGERR("failed to create DB (%s)", sqlite3_errmsg(s_db));
        return 0;
    }

    return 1;
}

int db_open(void)
{
    int err;
    const char *tail;

    static const char query_share_upd[] =
            "UPDATE files SET name=?,ext=?,size=?,type=?,mlength=?,mbitrate=?,mcodec=? WHERE fid=?";
    static const char query_share_ins[] =
            "INSERT OR REPLACE INTO files(fid,hash,name,ext,size,type,mlength,mbitrate,mcodec) "
                    "   VALUES(?,?,?,?,?,?,?,?,?)";
    static const char query_share_src[] =
            "INSERT INTO sources(fid,sid,complete,rating) VALUES(?,?,?,?)";
    static const char query_remove_src[] =
            "DELETE FROM sources WHERE sid=?";
    static const char query_get_src[] =
            "SELECT sid FROM sources WHERE fid=? LIMIT ?";

    err = sqlite3_open_v2(DB_NAME, &s_db, DB_OPEN_FLAGS, NULL);
    if (SQLITE_OK != err) {
        ED2KD_LOGERR("failed to open DB (%s)", sqlite3_errmsg(s_db));
        return 0;
    }

    DB_CHECK(SQLITE_OK == sqlite3_prepare_v2(s_db, query_share_upd, sizeof(query_share_upd), &s_stmt[SHARE_UPD], &tail));
    DB_CHECK(SQLITE_OK == sqlite3_prepare_v2(s_db, query_share_ins, sizeof(query_share_ins), &s_stmt[SHARE_INS], &tail));
    DB_CHECK(SQLITE_OK == sqlite3_prepare_v2(s_db, query_share_src, sizeof(query_share_src), &s_stmt[SHARE_SRC], &tail));
    DB_CHECK(SQLITE_OK == sqlite3_prepare_v2(s_db, query_remove_src, sizeof(query_remove_src), &s_stmt[REMOVE_SRC], &tail));
    DB_CHECK(SQLITE_OK == sqlite3_prepare_v2(s_db, query_get_src, sizeof(query_get_src), &s_stmt[GET_SRC], &tail));

    return 1;

    failed:
    db_close();
    return 0;
}

int db_destroy(void)
{
    return SQLITE_OK == sqlite3_close(s_db);
}

int db_close(void)
{
    size_t i;

    for (i = 0; i < STMT_COUNT; ++i) {
        if (s_stmt[i])
            sqlite3_finalize(s_stmt[i]);
    }

    return SQLITE_OK == sqlite3_close(s_db);
}

int db_share_files(const struct pub_file *files, size_t count, const struct client *owner)
{
    /* todo: do it in transaction */

    while (count-- > 0) {
        sqlite3_stmt *stmt;
        const char *ext;
        int ext_len;
        int i;
        uint64_t fid;

        if (!files->name_len) {
            files++;
            continue;
        }

        fid = MAKE_FID(files->hash);

        // find extension
        ext = file_extension(files->name, files->name_len);
        if (ext)
            ext_len = files->name + files->name_len - ext;
        else
            ext_len = 0;

        i = 1;
        stmt = s_stmt[SHARE_UPD];
        DB_CHECK(SQLITE_OK == sqlite3_reset(stmt));
        DB_CHECK(SQLITE_OK == sqlite3_bind_text(stmt, i++, files->name, files->name_len, SQLITE_STATIC));
        DB_CHECK(SQLITE_OK == sqlite3_bind_text(stmt, i++, ext, ext_len, SQLITE_STATIC));
        DB_CHECK(SQLITE_OK == sqlite3_bind_int64(stmt, i++, files->size));
        DB_CHECK(SQLITE_OK == sqlite3_bind_int(stmt, i++, files->type));
        DB_CHECK(SQLITE_OK == sqlite3_bind_int(stmt, i++, files->media_length));
        DB_CHECK(SQLITE_OK == sqlite3_bind_int(stmt, i++, files->media_bitrate));
        DB_CHECK(SQLITE_OK == sqlite3_bind_text(stmt, i++, files->media_codec, files->media_codec_len, SQLITE_STATIC));
        DB_CHECK(SQLITE_OK == sqlite3_bind_int64(stmt, i++, fid));
        DB_CHECK(SQLITE_DONE == sqlite3_step(stmt));

        if (!sqlite3_changes(s_db)) {
            i = 1;
            stmt = s_stmt[SHARE_INS];
            DB_CHECK(SQLITE_OK == sqlite3_reset(stmt));
            DB_CHECK(SQLITE_OK == sqlite3_bind_int64(stmt, i++, fid));
            DB_CHECK(SQLITE_OK == sqlite3_bind_blob(stmt, i++, files->hash, sizeof(files->hash), SQLITE_STATIC));
            DB_CHECK(SQLITE_OK == sqlite3_bind_text(stmt, i++, files->name, files->name_len, SQLITE_STATIC));
            DB_CHECK(SQLITE_OK == sqlite3_bind_text(stmt, i++, ext, ext_len, SQLITE_STATIC));
            DB_CHECK(SQLITE_OK == sqlite3_bind_int64(stmt, i++, files->size));
            DB_CHECK(SQLITE_OK == sqlite3_bind_int(stmt, i++, files->type));
            DB_CHECK(SQLITE_OK == sqlite3_bind_int(stmt, i++, files->media_length));
            DB_CHECK(SQLITE_OK == sqlite3_bind_int(stmt, i++, files->media_bitrate));
            DB_CHECK(SQLITE_OK == sqlite3_bind_text(stmt, i++, files->media_codec, files->media_codec_len, SQLITE_STATIC));
            DB_CHECK(SQLITE_DONE == sqlite3_step(stmt));
        }

        i = 1;
        stmt = s_stmt[SHARE_SRC];
        DB_CHECK(SQLITE_OK == sqlite3_reset(stmt));
        DB_CHECK(SQLITE_OK == sqlite3_bind_int64(stmt, i++, fid));
        DB_CHECK(SQLITE_OK == sqlite3_bind_int64(stmt, i++, MAKE_SID(owner)));
        DB_CHECK(SQLITE_OK == sqlite3_bind_int(stmt, i++, files->complete));
        DB_CHECK(SQLITE_OK == sqlite3_bind_int(stmt, i++, files->rating));
        DB_CHECK(SQLITE_DONE == sqlite3_step(stmt));

        files++;
    }

    return 1;

    failed:
    ED2KD_LOGERR("failed to add file to db (%s)", sqlite3_errmsg(s_db));
    return 0;
}

int db_remove_source(const struct client *clnt)
{
    sqlite3_stmt *stmt = s_stmt[REMOVE_SRC];

    DB_CHECK(SQLITE_OK == sqlite3_reset(stmt));
    DB_CHECK(SQLITE_OK == sqlite3_bind_int64(stmt, 1, MAKE_SID(clnt)));
    DB_CHECK(SQLITE_DONE == sqlite3_step(stmt));
    return 1;

    failed:
    ED2KD_LOGERR("failed to remove sources from db (%s)", sqlite3_errmsg(s_db));
    return 0;
}

int db_search_files(struct search_node *snode, struct evbuffer *buf, size_t *count)
{
    int err;
    const char *tail;
    sqlite3_stmt *stmt = 0;
    size_t i;
    struct {
        char name_term[MAX_NAME_TERM_LEN + 1];
        size_t name_len;
        uint64_t minsize;
        uint64_t maxsize;
        uint64_t srcavail;
        uint64_t srccomplete;
        uint64_t minbitrate;
        uint64_t minlength;
        struct search_node *ext_node;
        struct search_node *codec_node;
        struct search_node *type_node;
    } params;
    char query[MAX_SEARCH_QUERY_LEN + 1] =
            " SELECT f.hash,f.name,f.size,f.type,f.ext,f.srcavail,f.srccomplete,f.rating,f.rated_count,"
                    "  (SELECT sid FROM sources WHERE fid=f.fid LIMIT 1) AS sid,"
                    "  f.mlength,f.mbitrate,f.mcodec "
                    " FROM fnames n"
                    " JOIN files f ON f.fid = n.docid"
                    " WHERE fnames MATCH ?";

    memset(&params, 0, sizeof params);

    while (snode) {
        if ((ST_AND <= snode->type) && (ST_NOT >= snode->type)) {
            if (!snode->left_visited) {
                if (snode->string_term) {
                    params.name_len++;
                    DB_CHECK(params.name_len < sizeof params.name_term);
                    strcat(params.name_term, "(");
                }
                snode->left_visited = 1;
                snode = snode->left;
                continue;
            } else if (!snode->right_visited) {
                if (snode->string_term) {
                    const char *oper = 0;
                    switch (snode->type) {
                        case ST_AND:
                            params.name_len += 5;
                            oper = " AND ";
                            break;
                        case ST_OR:
                            params.name_len += 4;
                            oper = " OR ";
                            break;
                        case ST_NOT:
                            params.name_len += 5;
                            oper = " NOT ";
                            break;

                        default:
                            DB_CHECK(0);
                    }
                    DB_CHECK(params.name_len < sizeof params.name_term);
                    strcat(params.name_term, oper);
                }
                snode->right_visited = 1;
                snode = snode->right;
                continue;
            } else {
                if (snode->string_term) {
                    params.name_len++;
                    DB_CHECK(params.name_len < sizeof params.name_term);
                    strcat(params.name_term, ")");
                }
            }
        } else {
            switch (snode->type) {
                case ST_STRING:
                    params.name_len += snode->str_len;
                    DB_CHECK(params.name_len < sizeof params.name_term);
                    strncat(params.name_term, snode->str_val, snode->str_len);
                    break;
                case ST_EXTENSION:
                    params.ext_node = snode;
                    break;
                case ST_CODEC:
                    params.codec_node = snode;
                    break;
                case ST_MINSIZE:
                    params.minsize = snode->int_val;
                    break;
                case ST_MAXSIZE:
                    params.maxsize = snode->int_val;
                    break;
                case ST_SRCAVAIL:
                    params.srcavail = snode->int_val;
                    break;
                case ST_SRCCOMLETE:
                    params.srccomplete = snode->int_val;
                    break;
                case ST_MINBITRATE:
                    params.minbitrate = snode->int_val;
                    break;
                case ST_MINLENGTH:
                    params.minlength = snode->int_val;
                    break;
                case ST_TYPE:
                    params.type_node = snode;
                    break;
                default:
                    DB_CHECK(0);
            }
        }

        snode = snode->parent;
    }

    if (params.ext_node) {
        strcat(query, " AND f.ext=?");
    }
    if (params.codec_node) {
        strcat(query, " AND f.mcodec=?");
    }
    if (params.minsize) {
        strcat(query, " AND f.size>?");
    }
    if (params.maxsize) {
        strcat(query, " AND f.size<?");
    }
    if (params.srcavail) {
        strcat(query, " AND f.srcavail>?");
    }
    if (params.srccomplete) {
        strcat(query, " AND f.srccomplete>?");
    }
    if (params.minbitrate) {
        strcat(query, " AND f.mbitrate>?");
    }
    if (params.minlength) {
        strcat(query, " AND f.mlength>?");
    }
    if (params.type_node) {
        strcat(query, " AND f.type=?");
    }
    strcat(query, " LIMIT ?");

    DB_CHECK(SQLITE_OK == sqlite3_prepare_v2(s_db, query, strlen(query) + 1, &stmt, &tail));

    i = 1;
    DB_CHECK(SQLITE_OK == sqlite3_bind_text(stmt, i++, params.name_term, params.name_len + 1, SQLITE_STATIC));

    if (params.ext_node) {
        DB_CHECK(SQLITE_OK == sqlite3_bind_text(stmt, i++, params.ext_node->str_val, params.ext_node->str_len, SQLITE_STATIC));
    }
    if (params.codec_node) {
        DB_CHECK(SQLITE_OK == sqlite3_bind_text(stmt, i++, params.codec_node->str_val, params.codec_node->str_len, SQLITE_STATIC));
    }
    if (params.minsize) {
        DB_CHECK(SQLITE_OK == sqlite3_bind_int64(stmt, i++, params.minsize));
    }
    if (params.maxsize) {
        DB_CHECK(SQLITE_OK == sqlite3_bind_int64(stmt, i++, params.maxsize));
    }
    if (params.srcavail) {
        DB_CHECK(SQLITE_OK == sqlite3_bind_int64(stmt, i++, params.srcavail));
    }
    if (params.srccomplete) {
        DB_CHECK(SQLITE_OK == sqlite3_bind_int64(stmt, i++, params.srccomplete));
    }
    if (params.minbitrate) {
        DB_CHECK(SQLITE_OK == sqlite3_bind_int64(stmt, i++, params.minbitrate));
    }
    if (params.minlength) {
        DB_CHECK(SQLITE_OK == sqlite3_bind_int64(stmt, i++, params.minlength));
    }
    if (params.type_node) {
        uint8_t type = get_ed2k_file_type(params.type_node->str_val, params.type_node->str_len);
        DB_CHECK(SQLITE_OK == sqlite3_bind_int(stmt, i++, type));
    }

    DB_CHECK(SQLITE_OK == sqlite3_bind_int(stmt, i++, *count));

    i = 0;
    while (((err = sqlite3_step(stmt)) == SQLITE_ROW) && (i < *count)) {
        struct search_file sfile;
        uint64_t sid;
        int col = 0;

        memset(&sfile, 0, sizeof sfile);

        sfile.hash = (const unsigned char *) sqlite3_column_blob(stmt, col++);

        sfile.name_len = sqlite3_column_bytes(stmt, col);
        sfile.name_len = sfile.name_len > MAX_FILENAME_LEN ? MAX_FILENAME_LEN : sfile.name_len;
        sfile.name = (const char *) sqlite3_column_text(stmt, col++);

        sfile.size = sqlite3_column_int64(stmt, col++);
        sfile.type = sqlite3_column_int(stmt, col++);

        sfile.ext_len = sqlite3_column_bytes(stmt, col);
        sfile.ext_len = sfile.ext_len > MAX_FILEEXT_LEN ? MAX_FILEEXT_LEN : sfile.ext_len;
        sfile.ext = (const char *) sqlite3_column_text(stmt, col++);

        sfile.srcavail = sqlite3_column_int(stmt, col++);
        sfile.srccomplete = sqlite3_column_int(stmt, col++);
        sfile.rating = sqlite3_column_int(stmt, col++);
        sfile.rated_count = sqlite3_column_int(stmt, col++);

        sid = sqlite3_column_int64(stmt, col++);
        sfile.client_id = GET_SID_ID(sid);
        sfile.client_port = GET_SID_PORT(sid);

        sfile.media_length = sqlite3_column_int(stmt, col++);
        sfile.media_bitrate = sqlite3_column_int(stmt, col++);

        sfile.media_codec_len = sqlite3_column_bytes(stmt, col);
        sfile.media_codec_len = sfile.media_codec_len > MAX_FILEEXT_LEN ? MAX_FILEEXT_LEN : sfile.media_codec_len;
        sfile.media_codec = (const char *) sqlite3_column_text(stmt, col++);

        write_search_file(buf, &sfile);

        ++i;
    }

    DB_CHECK((i == *count) || (SQLITE_DONE == err));

    *count = i;
    return 1;

    failed:
    if (stmt) sqlite3_finalize(stmt);
    ED2KD_LOGERR("failed perform search query (%s)", sqlite3_errmsg(s_db));

    return 0;
}

int db_get_sources(const unsigned char *hash, struct file_source *sources, uint8_t *count)
{
    sqlite3_stmt *stmt = s_stmt[GET_SRC];
    uint8_t i;
    int err;

    DB_CHECK(SQLITE_OK == sqlite3_reset(stmt));
    DB_CHECK(SQLITE_OK == sqlite3_bind_int64(stmt, 1, MAKE_FID(hash)));
    DB_CHECK(SQLITE_OK == sqlite3_bind_int(stmt, 2, *count));

    i = 0;
    while (((err = sqlite3_step(stmt)) == SQLITE_ROW) && (i < *count)) {
        uint64_t sid = sqlite3_column_int64(stmt, 0);
        sources[i].ip = GET_SID_ID(sid);
        sources[i].port = GET_SID_PORT(sid);
        ++i;
    }

    DB_CHECK((i == *count) || (SQLITE_DONE == err));

    *count = i;
    return 1;

    failed:
    ED2KD_LOGERR("failed to get sources from db (%s)", sqlite3_errmsg(s_db));
    return 0;
}
