/*
 * Copyright (C) 2007,2008 Cybozu Labs, Inc.
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

extern "C" {
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
}
#include <algorithm>
#include <functional>
#include <list>
#include <vector>

#define MYSQL_SERVER

#include "mysql_priv.h"
#undef PACKAGE
#undef VERSION
#undef HAVE_DTRACE
#undef _DTRACE_VERSION
#include <mysql/plugin.h>

#include "queue_config.h"

#if SIZEOF_OFF_T != 8
#  error "support for 64-bit file offsets is mandatory"
#endif
#ifdef HAVE_LSEEK64
#  define lseek  lseek64
#  define pread  pread64
#  define pwrite pwrite64
#endif

#include "ha_queue.h"
#include "adler32.c"

extern uint build_table_filename(char *buff, size_t bufflen, const char *db,
				 const char *table, const char *ext,
				 uint flags);


using namespace std;

#define MIN_ROWS_BUFFER_SIZE (4096)
#define COMPACT_THRESHOLD (16777216)
#define EXPAND_BY (1048576)

#define DO_COMPACT(all, free) \
    ((all) >= COMPACT_THRESHOLD && (free) * 2 >= (all))
#define Q4M ".Q4M"
#define Q4T ".Q4T"

static HASH queue_open_tables;
static pthread_mutex_t open_mutex, listener_mutex;

#ifdef USE_RELATIVE_TIMEDWAIT
# ifdef SAFE_MUTEX
static int safe_cond_timedwait_relative_np(pthread_cond_t *cond,
					   safe_mutex_t *mp,
					   struct timespec *abstime,
					   const char *file, uint line);
#define pthread_cond_timedwait_relative_np(A,B,C) safe_cond_timedwait_relative_np((A),(B),(C),__FILE__,__LINE__)
# elif defined(MY_PTHREAD_FASTMUTEX)
#define pthread_cond_timedwait_relative_np(A,B,C) pthread_cond_timedwait_relative_np((A),&(B)->mutex,(C))
# endif
#endif

static handlerton *queue_hton;

static void log(const char *fmt, ...) __attribute__((format(printf,1,2)));
static void kill_proc(const char *fmt, ...) __attribute__((format(printf,1,2)));

static void vlog(const char *fmt, va_list args)
{
  fputs("ha_queue: ", stderr);
  vfprintf(stderr, fmt, args);
}

static void log(const char *fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  vlog(fmt, args);
  va_end(args);
}

static void kill_proc(const char *fmt, ...)
{
  va_list args;
  
  va_start(args, fmt);
  vlog(fmt, args);
  va_end(args);
  
  abort();
  static char *np = NULL;
  if (*np) {
    *np = NULL;
  }
}

static void sync_file(int fd)
{
  if (
#ifdef FDATASYNC_USE_FCNTL
      fcntl(fd, F_FULLFSYNC, 0) != 0
#elif defined(FDATASYNC_USE_FSYNC)
      fsync(fd) != 0
#elif defined(FDATASYNC_SKIP)
      0
#else
      fdatasync(fd) != 0
#endif
      ) {
    kill_proc("failed to sync disk\n");
  }
}

my_off_t queue_row_t::validate_checksum(int fd, my_off_t off)
{
  my_off_t off_end;
  char _len[sizeof(my_off_t)];
  
  /* read checksum size */
  off += queue_row_t::header_size();
  if (::pread(fd, _len, sizeof(_len), off) != sizeof(_len)) {
    return 0;
  }
  off += sizeof(_len);
  off_end = off + uint8korr(_len);
  /* calc checksum */
  uint32_t adler = 1;
  while (off != off_end) {
    /* read header */
    queue_row_t r;
    if (off_end - off < header_size()
	|| ::pread(fd, &r, header_size(), off)
	!= static_cast<ssize_t>(header_size())) {
      return 0;
    }
    if (r.type() == type_checksum) {
      return 0;
    } else if (r.type() == (type_flag_removed | type_row)
	       || r.type() == (type_flag_removed | type_row_received)) {
      r.set_type(r.type() & ~type_flag_removed);
    }
    adler = adler32(adler, &r, header_size());
    off += header_size();
    /* read data */
    my_off_t row_end = off + r.size();
    if (row_end > off_end) {
      return 0;
    }
    while (off != row_end) {
      char buf[4096];
      ssize_t bs = min(row_end - off, sizeof(buf));
      if (::pread(fd, buf, bs, off) != bs) {
	return 0;
      }
      adler = adler32(adler, buf, bs);
      off += bs;
    }
  }
  /* compare checksum */
  return size() == (adler & size_mask) ? off : 0;
}

queue_row_t *queue_row_t::create_checksum(const iovec* iov, int iovcnt)
{
  my_off_t sz = 0;
  uint32_t adler = 1;
  
  for (int i = 0; i < iovcnt; i++) {
    adler = adler32(adler, iov[i].iov_base, iov[i].iov_len);
    sz += iov[i].iov_len;
  }
  
  queue_row_t *row =
    static_cast<queue_row_t*>(my_malloc(checksum_size(), MYF(0)));
  assert(row != NULL);
  int4store(row->_size, type_checksum | (adler & size_mask));
  int8store(row->_bytes, sz);
  
  return row;
}

queue_file_header_t::queue_file_header_t()
{
  int4store(_magic, MAGIC_V2);
  int4store(_attr, 0);
  int8store(_end, static_cast<my_off_t>(sizeof(queue_file_header_t)));
  int8store(_begin, static_cast<my_off_t>(sizeof(queue_file_header_t)));
  int8store(_compaction_offset, 0LL);
  memset(_last_received_offsets, 0, sizeof(_last_received_offsets));
  memset(_padding, 0, sizeof(_padding));
}

void queue_file_header_t::write(int fd)
{
  if (pwrite(fd, this, sizeof(*this), 0) != sizeof(*this)) {
    kill_proc("failed to update header\n");
  }
}

uchar* queue_share_t::get_share_key(queue_share_t *share, size_t *length,
				    my_bool not_used __attribute__((unused)))
{
  *length = share->table_name_length;
  return reinterpret_cast<uchar*>(share->table_name);
}

void queue_share_t::fixup_header()
{
  /* update end */
  my_off_t off = _header.end();
  while (1) {
    queue_row_t row;
    if (read(&row, off, queue_row_t::header_size(), true)
	!= static_cast<ssize_t>(queue_row_t::header_size())) {
      break;
    }
    if (row.type() != queue_row_t::type_checksum) {
      break;
    }
    if ((off = row.validate_checksum(fd, off)) == 0) {
      break;
    }
    _header.set_end(off);
  }
  /* update last_received_offsets */
  off = _header.begin();
  while (off < _header.end()) {
    queue_row_t row;
    if (read(&row, off, queue_row_t::header_size(), true)
	!= static_cast<ssize_t>(queue_row_t::header_size())) {
      kill_proc("I/O error: %s\n", table_name);
    }
    if ((row.type() & ~queue_row_t::type_flag_removed)
	== queue_row_t::type_row_received) {
      queue_source_t source(0, 0);
      if (read(&source,
	       off + queue_row_t::header_size() + row.size() - sizeof(source),
	       sizeof(source), true)
	  != sizeof(source)) {
	kill_proc("corrupt table: %s\n", table_name);
      }
      if (source.sender() > QUEUE_MAX_SOURCES) {
	kill_proc("corrupt table: %s\n", table_name);
      }
      _header.set_last_received_offset(source.sender(), source.offset());
    }
    off = row.next(off);
  }
  /* update begin */
  off = _header.begin();
  while (off < _header.end()) {
    queue_row_t row;
    if (read(&row, off, queue_row_t::header_size(), true)
	!= static_cast<ssize_t>(queue_row_t::header_size())) {
      kill_proc("I/O error: %s\n", table_name);
    }
    if (row.type() == queue_row_t::type_row
	|| row.type() == queue_row_t::type_row_received) {
      break;
    }
    off = row.next(off);
  }
  _header.set_begin(off);
  /* save */
  _header.set_attr(_header.attr() & ~queue_file_header_t::attr_is_dirty);
  _header.write(fd);
  sync_file(fd);
}

static bool load_table(TABLE *table, const char *db_table_name)
{
  // precondition: LOCK_open should be acquired
  
  TABLE_SHARE *share;
  TABLE_LIST table_list;
  char key[MAX_DBKEY_LENGTH];
  uint key_length;
  int err;
  char *db_table_buf;
  
  bzero((char*)&table_list, sizeof(TABLE_LIST));
  bzero((char*)table, sizeof(TABLE));
  
  /* copy table name to buffer and split to db name and table name */
  if ((db_table_buf = strdup(db_table_name)) == NULL) {
    log("out of memory\n");
    return false;
  }
  for (table_list.db = db_table_buf;
       *table_list.db == '/' || *table_list.db == '.';
       table_list.db++)
    ;
  if (*table_list.db == '\0') {
    log("invalid table name: %s\n", db_table_name);
    goto Error;
  }
  for (table_list.table_name = table_list.db + 1;
       *table_list.table_name != '/';
       table_list.table_name++) {
    if (*table_list.table_name == '\0') {
      log("invalid table name: %s\n", db_table_name);
      goto Error;
    }
  }
  *table_list.table_name++ = '\0';
  
  /* load table data */
  key_length = create_table_def_key(current_thd, key, &table_list, 0);
  if ((share = get_table_share(current_thd, &table_list, key, key_length, 0,
			       &err))
      == NULL) {
    return true;
  }
  if (open_table_from_share(current_thd, share, table_list.table_name, 0,
			    READ_ALL, 0, table, FALSE)
      != 0) {
    goto Error;
  }
  
  /* free and return */
  free(db_table_buf);
  return true;
  
 Error:
  free(db_table_buf);
  return false;
}

queue_share_t *queue_share_t::get_share(const char *table_name)
{
  queue_share_t *share;
  uint table_name_length;
  char *tmp_name;
  char filename[FN_REFLEN];
  
  pthread_mutex_lock(&open_mutex);
  
  table_name_length = strlen(table_name);
  
  /* return the one, if found (after incrementing refcount) */
  if ((share = reinterpret_cast<queue_share_t*>(hash_search(&queue_open_tables, reinterpret_cast<const uchar*>(table_name), table_name_length)))
      != NULL) {
    ++share->use_count;
    pthread_mutex_unlock(&open_mutex);
    return share;
  }
  
  /* alloc */
  if (my_multi_malloc(MYF(MY_WME | MY_ZEROFILL), &share, sizeof(queue_share_t),
		      &tmp_name, table_name_length + 1, NullS)
      == NULL) {
    goto ERR_RETURN;
  }
  
  /* init members that would always succeed in doing so */
  share->use_count = 1;
  share->table_name = tmp_name;
  strmov(share->table_name, table_name);
  share->table_name_length = table_name_length;
  pthread_mutex_init(&share->mutex, MY_MUTEX_INIT_FAST);
  thr_lock_init(&share->store_lock);
  share->cache.off = 0;
  new (&share->rows_owned) queue_rows_owned_t();
  new (&share->listener_list) listener_list_t();
  share->num_readers = 0;
  pthread_cond_init(&share->to_writer_cond, NULL);
  pthread_cond_init(&share->_from_writer_conds[0], NULL);
  pthread_cond_init(&share->_from_writer_conds[1], NULL);
  share->from_writer_cond = &share->_from_writer_conds[0];
  share->writer_exit = false;
  share->append_list = new append_list_t();
  share->remove_list = new remove_list_t();
  new (&share->cond_eval) queue_cond_t();
  new (&share->active_cond_expr_list) cond_expr_list_t();
  new (&share->inactive_cond_expr_list) cond_expr_list_t();
  new (&share->cond_expr_true)
    cond_expr_t(new queue_cond_t::const_node_t
		(queue_cond_t::value_t::int_value(1)), "1", 1, 0);
  /* open file */
  fn_format(filename, share->table_name, "", Q4M,
	    MY_REPLACE_EXT | MY_UNPACK_FILENAME);
  if ((share->fd = open(filename, O_RDWR, 0)) == -1) {
    goto ERR_ON_FILEOPEN;
  }
  /* load header */
  if (::pread(share->fd, &share->_header, sizeof(share->_header), 0)
      != sizeof(share->_header)) {
    goto ERR_AFTER_FILEOPEN;
  }
  switch (share->_header.magic()) {
  case queue_file_header_t::MAGIC_V1:
  case queue_file_header_t::MAGIC_V2:
    break;
  default:
    goto ERR_AFTER_FILEOPEN;
  }
  /* sanity check */
  if ((share->_header.attr() & queue_file_header_t::attr_is_dirty) != 0) {
    share->fixup_header();
  }
  /* set dirty flag */
  share->_header.set_attr(share->_header.attr()
			  | queue_file_header_t::attr_is_dirty);
  share->_header.write(share->fd);
  sync_file(share->fd);
  /* seek to end position for inserts */
  if (lseek(share->fd, share->_header.end(), SEEK_SET) == -1) {
    goto ERR_AFTER_FILEOPEN;
  }
  /* start writer thread */
  if (pthread_create(&share->writer_thread, NULL, _writer_start, share) != 0) {
    goto ERR_AFTER_FILEOPEN;
  }
  /* add to open_tables */
  if (my_hash_insert(&queue_open_tables, reinterpret_cast<uchar*>(share))) {
    goto ERR_AFTER_WRITER_START;
  }
  
  /* success, unlock */
  pthread_mutex_unlock(&open_mutex);
  
  return share;
  
 ERR_AFTER_WRITER_START:
  share->writer_exit = true;
  pthread_cond_signal(&share->to_writer_cond);
  pthread_join(share->writer_thread, NULL);
 ERR_AFTER_FILEOPEN:
  close(share->fd);
 ERR_ON_FILEOPEN:
  share->cond_expr_true.free_data();
  share->inactive_cond_expr_list.~cond_expr_list_t();
  share->active_cond_expr_list.~cond_expr_list_t();
  share->cond_eval.~queue_cond_t();
  delete share->remove_list;
  delete share->append_list;
  pthread_cond_destroy(&share->_from_writer_conds[0]);
  pthread_cond_destroy(&share->_from_writer_conds[1]);
  pthread_cond_destroy(&share->to_writer_cond);
  share->listener_list.~list();
  share->rows_owned.~list();
  thr_lock_delete(&share->store_lock);
  pthread_mutex_destroy(&share->mutex);
  my_free(reinterpret_cast<uchar*>(share), MYF(0));
 ERR_RETURN:
  pthread_mutex_unlock(&open_mutex);
  return NULL;
  }

void queue_share_t::release()
{
  pthread_mutex_lock(&open_mutex);
  
  if (--use_count == 0) {
    delete [] fixed_buf;
    for (size_t i = 0; i < fields; i++) {
      delete fixed_fields[i];
    }
    delete [] fixed_fields;
    hash_delete(&queue_open_tables, reinterpret_cast<uchar*>(this));
    writer_exit = true;
    pthread_cond_signal(&to_writer_cond);
    if (pthread_join(writer_thread, NULL) != 0) {
      kill_proc("failed to join writer thread\n");
    }
    _header.write(fd);
    sync_file(fd);
    _header.set_attr(_header.attr() & ~queue_file_header_t::attr_is_dirty);
    _header.write(fd);
    sync_file(fd);
    close(fd);
    cond_expr_true.free_data();
    for (cond_expr_list_t::iterator i = inactive_cond_expr_list.begin();
	 i != inactive_cond_expr_list.end();
	 ++i) {
      i->free_data();
    }
    inactive_cond_expr_list.~cond_expr_list_t();
    active_cond_expr_list.~cond_expr_list_t();
    cond_eval.~queue_cond_t();
    delete remove_list;
    delete append_list;
    pthread_cond_destroy(&_from_writer_conds[0]);
    pthread_cond_destroy(&_from_writer_conds[1]);
    pthread_cond_destroy(&to_writer_cond);
    listener_list.~list();
    rows_owned.~list();
    thr_lock_delete(&store_lock);
    pthread_mutex_destroy(&mutex);
    my_free(reinterpret_cast<uchar*>(this), MYF(0));
  }
  
  pthread_mutex_unlock(&open_mutex);
}

bool queue_share_t::init_fixed_fields(TABLE *_table)
{
  if (fixed_fields != NULL) {
    return true;
  }
  
  /* Lock and load table information if not given.   The lock order should
   * always be LOCK_open -> queue_share_t::lock. */
  TABLE *table;
  TABLE table_buf;
  if (_table == NULL) {
    pthread_mutex_lock(&LOCK_open);
    lock();
    if (fixed_fields != NULL) {
      unlock();
      pthread_mutex_unlock(&LOCK_open);
      return true;
    }
    if (! load_table(&table_buf, table_name)) {
      unlock();
      pthread_mutex_unlock(&LOCK_open);
      return false;
    }
    table = &table_buf;
  } else {
    lock();
    if (fixed_fields != NULL) {
      unlock();
      return true;
    }
    table = _table;
  }
  
  /* setup fixed_fields */
  fixed_fields = new queue_fixed_field_t* [table->s->fields];
  if (_header.magic() == queue_file_header_t::MAGIC_V2) {
    Field **field;
    int field_index;
    size_t off = table->s->null_bytes;
    for (field = table->field, field_index = 0;
	 *field != NULL;
	 field++, field_index++) {
      switch ((*field)->type()) {
#define TYPEMAP(type, cls) \
	case MYSQL_TYPE_##type:	\
	  fixed_fields[field_index] = new cls; \
          off += fixed_fields[field_index]->size(); \
	  break
	TYPEMAP(TINY, queue_int_field_t<1>(table, *field));
	TYPEMAP(SHORT, queue_int_field_t<2>(table, *field));
	TYPEMAP(INT24, queue_int_field_t<3>(table, *field));
	TYPEMAP(LONG, queue_int_field_t<4>(table, *field));
	TYPEMAP(LONGLONG, queue_int_field_t<8>(table, *field));
	TYPEMAP(FLOAT, queue_fixed_field_t(table, *field, sizeof(float)));
	TYPEMAP(DOUBLE, queue_fixed_field_t(table, *field, sizeof(double)));
	TYPEMAP(TIMESTAMP, queue_int_field_t<4>(table, *field));
	TYPEMAP(DATE, queue_int_field_t<4>(table, *field));
	TYPEMAP(NEWDATE, queue_int_field_t<3>(table, *field));
	TYPEMAP(TIME, queue_int_field_t<3>(table, *field));
	TYPEMAP(DATETIME, queue_int_field_t<8>(table, *field));
#undef TYPEMAP
      default:
	fixed_fields[field_index] = NULL;
	break;
      }
    }
  } else {
    fill(fixed_fields, fixed_fields + table->s->fields,
	 static_cast<queue_fixed_field_t*>(NULL));
  }
  /* setup other fields */
  null_bytes = table->s->null_bytes;
  fields = table->s->fields;
  fixed_buf_size = null_bytes;
  for (size_t i = 0; i < fields; i++) {
    const queue_fixed_field_t *field = fixed_fields[i];
    if (field != NULL && field->is_convertible()) {
      cond_eval.add_column(field->name());
      fixed_buf_size += field->size();
    }
  }
  fixed_buf = new uchar [fixed_buf_size];
  
  /* unlock */
  unlock();
  if (_table == NULL) {
    closefrm(table, true);
    pthread_mutex_unlock(&LOCK_open);
  }
  
  return true;
}

void queue_share_t::unlock_reader()
{
  lock();
  if (--num_readers == 0
      && DO_COMPACT(_header.end() - sizeof(_header),
		    _header.begin() - sizeof(_header))) {
    pthread_cond_signal(&to_writer_cond);
  }
  unlock();
}

void queue_share_t::unregister_listener(listener_t *l)
{
  for (listener_list_t::iterator i = listener_list.begin();
       i != listener_list.end();
       ++i) {
    if (i->first == l) {
      listener_list.erase(i);
      break;
    }
  }
}

void queue_share_t::wake_listeners()
{
  bool use_cond_expr = false;
  my_off_t off = (my_off_t)-1;
  
  // note: lock order should always be: listener_mutex -> queue_share_t::mutex
  pthread_mutex_lock(&listener_mutex);
  
  // remove listeners with signals received
  listener_list_t::iterator l = listener_list.begin();
  while (l != listener_list.end()) {
    if (l->first->signalled_by != NULL) {
      l = listener_list.erase(l);
    } else {
      if (l->second != &cond_expr_true) {
	use_cond_expr = true;
      }
      off = min(off, l->second->pos);
      ++l;
    }
  }
  if (listener_list.size() == 0) {
    goto UNLOCK_G_RETURN;
  }
  
  // per-row test
  lock();
  if (off == 0) {
    off = _header.begin();
  } else if (next(&off) != 0) {
    log("internal error, table corrupt?\n");
    goto UNLOCK_ALL_RETURN;
  }
  while (off != _header.end()) {
    while (find_owner(off) != 0) {
      if (next(&off) != 0) {
	log("internal error, table corrupt?\n");
	goto UNLOCK_ALL_RETURN;
      }
    }
    if (use_cond_expr && setup_cond_eval(off) != 0) {
      log("internal error, table corrupt?\n");
      goto UNLOCK_ALL_RETURN;
    }
    for (l = listener_list.begin(); l != listener_list.end(); ++l) {
      bool found = false;
      if (l->second == &cond_expr_true) {
	found = true;
      } else if (l->second->pos < off) {
	l->second->pos = off;
	if (cond_eval.evaluate(l->second->node)) {
	  found = true;
	}
      }
      if (found) {
	rows_owned.push_back(make_pair(l->first->listener, off));
	l->first->signalled_by = this;
	pthread_cond_signal(&l->first->cond);
	listener_list.erase(l);
	if (listener_list.size() == 0) {
	  goto UNLOCK_ALL_RETURN;
	}
	break;
      }
    }
    if (next(&off) != 0) {
      log("internal error, table corrupt?\n");
      goto UNLOCK_ALL_RETURN;
    }
  }
 UNLOCK_ALL_RETURN:
  unlock();
  
 UNLOCK_G_RETURN:
  pthread_mutex_unlock(&listener_mutex);
}

struct queue_reset_owner_update_cond_expr {
  queue_share_t *share;
  my_off_t off;
  queue_reset_owner_update_cond_expr(queue_share_t *s, my_off_t o)
  : share(s), off(o) {}
  void operator()(queue_share_t::cond_expr_t& e) const {
    if (off <= e.pos && share->cond_eval.evaluate(e.node)) {
      // todo: should find a way to obtain prev. row
      e.pos = 0;
    }
  }
};

my_off_t queue_share_t::reset_owner(pthread_t owner)
{
  my_off_t off = 0;
  lock();
  
  // find the row to be released, and remove it from owner list
  for (queue_rows_owned_t::iterator i = rows_owned.begin();
       i != rows_owned.end();
       ++i) {
    if (i->first == owner) {
      off = i->second;
      rows_owned.erase(i);
      break;
    }
  }
  // update positions of cond_expr_list
  if (off != 0) {
    if (setup_cond_eval(off) == 0) {
      apply_cond_expr_list(queue_reset_owner_update_cond_expr(this, off));
    }
  }
  
  unlock();
  return off;
}

int queue_share_t::write_rows(const void *rows, size_t rows_size)
{
  queue_connection_t *conn = queue_connection_t::current();
  queue_source_t *source =
    conn != NULL && conn->source.offset() != 0 ? &conn->source : NULL;
  
  append_t a(rows, rows_size, source);
  
  pthread_mutex_lock(&mutex);
  if (source != NULL && ! conn->reset_source
      && source->offset() <= _header.last_received_offset(source->sender())) {
    pthread_mutex_unlock(&mutex);
    log("skipping forwarded duplicates: %s,max %llu,got %llu\n", table_name,
	_header.last_received_offset(source->sender()), source->offset());
    *source = queue_source_t(0, 0);
    return QUEUE_ERR_RECORD_EXISTS;
  }
  append_list->push_back(&a);
  pthread_cond_t *c = from_writer_cond;
  pthread_cond_signal(&to_writer_cond);
  do {
    pthread_cond_wait(c, &mutex);
  } while (a.err == -1);
  pthread_mutex_unlock(&mutex);
  
  if (source != NULL) {
    *source = queue_source_t(0, 0);
  }
  return a.err;
}

const void *queue_share_t::read_cache(my_off_t off, ssize_t size,
				      bool populate_cache)
{
  if (size > static_cast<ssize_t>(sizeof(cache.buf))) {
    return NULL;
  }
  if (cache.off != 0
      && cache.off <= off && off + size <= cache.off + sizeof(cache.buf)) {
    return cache.buf + off - cache.off;
  }
  if (! populate_cache) {
    return NULL;
  }
  if (pread(fd, cache.buf, sizeof(cache.buf), off) < size) {
    cache.off = 0; // invalidate
    return NULL;
  }
  cache.off = off;
  return cache.buf;
}

ssize_t queue_share_t::read(void *data, my_off_t off, ssize_t size,
			    bool populate_cache)
{
  const void* cp;
  if ((cp = read_cache(off, size, populate_cache)) != NULL) {
    memcpy(data, cp, size);
    return size;
  }
  return pread(fd, data, size, off);
}

int queue_share_t::next(my_off_t *off)
{
  if (*off == _header.end()) {
    return 0;
  }
  queue_row_t row;
  if (read(&row, *off, queue_row_t::header_size(), true)
      != static_cast<ssize_t>(queue_row_t::header_size())) {
    return -1;
  }
  *off = row.next(*off);
  while (1) {
    if (*off == _header.end()) {
      return 0;
    }
    if (read(&row, *off, queue_row_t::header_size(), true)
	!= static_cast<ssize_t>(queue_row_t::header_size())) {
      return -1;
    }
    switch (row.type()) {
    case queue_row_t::type_row:
    case queue_row_t::type_row_received:
      return 0;
    }
    *off = row.next(*off);
  }
}

my_off_t queue_share_t::get_owned_row(pthread_t owner, bool remove)
{
  for (queue_rows_owned_t::iterator i = rows_owned.begin();
       i != rows_owned.end();
       ++i) {
    if (i->first == owner) {
      my_off_t off = i->second;
      if (remove) {
	rows_owned.erase(i);
      }
      return off;
    }
  }
  return 0;
}

int queue_share_t::remove_rows(my_off_t *offsets, int cnt)
{
  remove_t r(offsets, cnt);
  
  remove_list->push_back(&r);
  pthread_cond_t *c = from_writer_cond;
  pthread_cond_signal(&to_writer_cond);
  do {
    pthread_cond_wait(c, &mutex);
  } while (r.err == -1);
  
  return r.err;
}

pthread_t queue_share_t::find_owner(my_off_t off)
{
  for (queue_rows_owned_t::const_iterator j = rows_owned.begin();
       j != rows_owned.end();
       ++j) {
    if (off == j->second) {
      return j->first;
    }
  }
  return 0;
}

my_off_t queue_share_t::assign_owner(pthread_t owner, cond_expr_t *cond_expr)
{
  my_off_t off = cond_expr->pos;
  if (off == 0) {
    off = _header.begin();
  } else if (next(&off) != 0) {
    return 0;
  }
  
  while (off != _header.end()) {
    cond_expr->pos = off;
    if (find_owner(off) == 0) {
      if (cond_expr == &cond_expr_true) {
	goto FOUND;
      } else {
	if (setup_cond_eval(off) != 0) {
	  log("internal error, table corrupt?");
	  return 0;
	}
	if (cond_eval.evaluate(cond_expr->node)) {
	  goto FOUND;
	}
      }
    }
    if (next(&off) != 0) {
      return 0;
    }
  }
  return 0;
  
 FOUND:
  rows_owned.push_back(queue_rows_owned_t::value_type(owner, off));
  return off;
}

int queue_share_t::setup_cond_eval(my_off_t pos)
{
  /* read row data */
  queue_row_t hdr;
  if (read(&hdr, pos, queue_row_t::header_size(), true)
      != static_cast<ssize_t>(queue_row_t::header_size())) {
    return HA_ERR_CRASHED_ON_USAGE;
  }
  if (read(fixed_buf, pos + queue_row_t::header_size(),
	   min(hdr.size(), fixed_buf_size), true)
      != static_cast<ssize_t>(min(hdr.size(), fixed_buf_size))) {
    return HA_ERR_CRASHED_ON_USAGE;
  }
  /* assign row data to evaluator */
  size_t col_index = 0, offset = null_bytes;
  for (size_t i = 0; i < fields; i++) {
    queue_fixed_field_t *field = fixed_fields[i];
    if (field != NULL) {
      if (field->is_null(fixed_buf)) {
	cond_eval.set_value(col_index++, queue_cond_t::value_t::null_value());
      } else {
	if (field->is_convertible()) {
	  cond_eval.set_value(col_index++, field->get_value(fixed_buf, offset));
	}
	offset += field->size();
      }
    }
  }
  return 0;
}

queue_share_t::cond_expr_t *
queue_share_t::compile_cond_expr(const char *expr, size_t len)
{
  if (expr == NULL) {
    return &cond_expr_true;
  }
  
  // return an existing one, if any
  for (cond_expr_list_t::iterator i = active_cond_expr_list.begin();
       i != active_cond_expr_list.end();
       ++i) {
    if (i->expr_len == len && memcmp(i->expr, expr, len) == 0) {
      i->ref_cnt++;
      return &*i;
    }
  }
  for (cond_expr_list_t::iterator i = inactive_cond_expr_list.begin();
       i != inactive_cond_expr_list.end();
       ++i) {
    if (i->expr_len == len && memcmp(i->expr, expr, len) == 0) {
      active_cond_expr_list.push_back(*i);
      inactive_cond_expr_list.erase(i);
      cond_expr_t *e = &active_cond_expr_list.back();
      e->ref_cnt++;
      return e;
    }
  }
  
  // compile and return
  queue_cond_t::node_t *n = cond_eval.compile_expression(expr, len);
  if (n == NULL) {
    return NULL;
  }
  active_cond_expr_list.push_back(cond_expr_t(n, expr, len, 0));
  return &active_cond_expr_list.back();
}

void queue_share_t::release_cond_expr(cond_expr_t *e)
{
  if (e != &cond_expr_true && --e->ref_cnt == 0) {
    for (cond_expr_list_t::iterator i = active_cond_expr_list.begin();
	 i != active_cond_expr_list.end();
	 ++i) {
      if (&*i == e) {
	inactive_cond_expr_list.push_back(*i);
	active_cond_expr_list.erase(i);
	break;
      }
    }
    while (inactive_cond_expr_list.size() >= 100) {
      inactive_cond_expr_list.front().free_data();
      inactive_cond_expr_list.pop_front();
    }
  }
}

static void close_append_list(queue_share_t::append_list_t *l, int err)
{
  for (queue_share_t::append_list_t::iterator i= l->begin();
       i != l->end();
       ++i) {
    (*i)->err = err;
  }
  delete l;
}

int queue_share_t::writer_do_append(append_list_t *l)
{
  /* build iovec */
  vector<iovec> iov;
  my_off_t total_len = 0;
  iov.push_back(iovec());
  for (append_list_t::iterator i = l->begin(); i != l->end(); ++i) {
    iov.push_back(iovec());
    iov.back().iov_base = const_cast<void*>((*i)->rows);
    total_len += iov.back().iov_len = (*i)->rows_size;
  }
  iov[0].iov_base =
    queue_row_t::create_checksum(&iov.front() + 1, iov.size() - 1);
  total_len += iov[0].iov_len =
    static_cast<queue_row_t*>(iov[0].iov_base)->next(0);
  /* expand if necessary */
  if ((_header.end() - 1) / EXPAND_BY
      != (_header.end() + total_len) / EXPAND_BY) {
    my_off_t new_len =
      ((_header.end() + total_len) / EXPAND_BY + 1) * EXPAND_BY;
    if (lseek(fd, new_len - 1, SEEK_SET) == -1
	|| write(fd, "", 1) != 1
	|| lseek(fd, _header.end(), SEEK_SET) == -1) {
      /* expansion failed */
      return HA_ERR_RECORD_FILE_FULL;
    }
  }
  { /* write and sync */
    vector<iovec>::const_iterator writev_from = iov.begin();
    ssize_t writev_len = writev_from->iov_len;
    for (vector<iovec>::const_iterator i = iov.begin() + 1;
	 i != iov.end();
	 ++i) {
      if (i - writev_from >= IOV_MAX
	  || writev_len + i->iov_len > SSIZE_MAX / 2) {
	if (writev(fd, &*writev_from, i - writev_from) != writev_len) {
	  my_free(iov[0].iov_base, MYF(0));
	  return HA_ERR_CRASHED_ON_USAGE;
	}
	writev_from = i;
	writev_len = 0;
      }
      writev_len += i->iov_len;
    }
    if (writev(fd, &*writev_from, iov.end() - writev_from) != writev_len) {
      my_free(iov[0].iov_base, MYF(0));
      return HA_ERR_CRASHED_ON_USAGE;
    }
    sync_file(fd);
  }
  /* update begin, end, cache, last_received_offset */
  pthread_mutex_lock(&mutex);
  if (_header.begin() == _header.end()) {
    _header.set_begin(_header.begin() + iov[0].iov_len);
  }
  for (vector<iovec>::const_iterator i = iov.begin(); i != iov.end(); ++i) {
    update_cache(i->iov_base, _header.end(), i->iov_len);
    _header.set_end(_header.end() + i->iov_len);
  }
  for (append_list_t::iterator i = l->begin(); i != l->end(); ++i) {
    const queue_source_t *s = (*i)->source;
    if (s != NULL) {
      _header.set_last_received_offset(s->sender(), s->offset());
    }
  }
  pthread_mutex_unlock(&mutex);
  
  my_free(iov[0].iov_base, MYF(0));
  return 0;
}

void queue_share_t::writer_do_remove(remove_list_t* l)
{
  remove_list_t::iterator i;
  int err = 0;
  
  for (remove_list_t::iterator i = l->begin(); i != l->end(); ++i) {
    /* lock mutex for each bulk delete, so as to make the deletion atomic to the
     * deleter (would not be atomic for non-owner selects, but who cares :-o */
    pthread_mutex_lock(&mutex);
    for (int j = 0; err == 0 && j < (*i)->cnt; j++) {
      queue_row_t row;
      my_off_t off = (*i)->offsets[j];
      if (read(&row, off, queue_row_t::header_size(), false)
	  == static_cast<ssize_t>(queue_row_t::header_size())) {
	row.set_type(row.type() | queue_row_t::type_flag_removed);
	if (pwrite(fd, &row, queue_row_t::header_size(), off)
	    != static_cast<ssize_t>(queue_row_t::header_size())) {
	  err = HA_ERR_CRASHED_ON_USAGE;
	}
	update_cache(&row, off, queue_row_t::header_size());
	if (_header.begin() == off) {
	  if (next(&off) == 0) {
	    _header.set_begin(off);
	  } else {
	    err = HA_ERR_CRASHED_ON_USAGE;
	  }
	}
      } else {
	err = HA_ERR_CRASHED_ON_USAGE;
      }
    }
    (*i)->err = err;
    pthread_mutex_unlock(&mutex);
  }
}

void *queue_share_t::writer_start()
{
  pthread_mutex_lock(&mutex);
  
  while (1) {
    /* wait for signal if we do not have any pending writes */
    while (remove_list->size() == 0 && append_list->size() == 0) {
      if (writer_exit) {
	goto EXIT;
      } else if (num_readers == 0
		 && DO_COMPACT(_header.end() - sizeof(_header),
			       _header.begin() - sizeof(_header))) {
	compact();
      } else {
	pthread_cond_wait(&to_writer_cond, &mutex);
      }
    }
    /* detach operation lists */
    remove_list_t *rl = NULL;
    append_list_t *al = NULL;
    if (remove_list->size() != 0) {
      rl = remove_list;
      remove_list = new remove_list_t();
    }
    if (append_list->size() != 0) {
      al = append_list;
      append_list = new append_list_t();
    }
    pthread_cond_t *notify_cond = from_writer_cond;
    from_writer_cond = _from_writer_conds + (_from_writer_conds == notify_cond);
    /* do the task and send back the results */
    pthread_mutex_unlock(&mutex);
    if (rl != NULL) {
      writer_do_remove(rl);
      delete rl;
    }
    if (al != NULL) {
      int err = 0;
      if ((err = writer_do_append(al)) != 0) {
	sync_file(fd);
      }
      close_append_list(al, err);
      pthread_cond_broadcast(notify_cond);
      wake_listeners();
    } else {
      sync_file(fd);
      pthread_cond_broadcast(notify_cond);
    }
    pthread_mutex_lock(&mutex);
  }
  
 EXIT:
  pthread_mutex_unlock(&mutex);
  return NULL;
}

static int copy_file_content(int src_fd, my_off_t begin, my_off_t end,
			     int dest_fd)
{
  char buf[65536];
  
  while (begin < end) {
    ssize_t bs = min(end - begin, sizeof(buf));
    if (pread(src_fd, buf, bs, begin) != bs
	|| write(dest_fd, buf, bs) != bs) {
      return -1;
    }
    begin += bs;
  }
  
  return 0;
}

struct queue_compact_update_cond_expr {
  my_off_t delta;
  queue_compact_update_cond_expr(my_off_t d) : delta(d) {}
  void operator()(queue_share_t::cond_expr_t& e) const {
    if (e.pos >= delta + sizeof(queue_file_header_t)) {
      e.pos -= delta;
    } else {
      e.pos = 0;
    }
  }
};

int queue_share_t::compact()
{
  log("starting table compaction: %s\n", table_name);
  
  char filename[FN_REFLEN], tmp_filename[FN_REFLEN];
  int tmp_fd;
  my_off_t delta = _header.begin() - sizeof(queue_file_header_t);
  
  /* open new file */
  fn_format(filename, table_name, "", Q4M,
	    MY_REPLACE_EXT | MY_UNPACK_FILENAME);
  fn_format(tmp_filename, table_name, "", Q4T,
	    MY_REPLACE_EXT | MY_UNPACK_FILENAME);
  if ((tmp_fd = open(tmp_filename, O_CREAT | O_TRUNC | O_RDWR, 0660))
      == -1) {
    log("failed to create temporary file: %s\n", tmp_filename);
    goto ERR_RETURN;
  }
  { /* write data (and seek to end) */
    queue_file_header_t hdr;
    hdr.set_end(_header.end() - delta);
    hdr.set_compaction_offset(_header.compaction_offset() + delta);
    for (int i = 0; i < QUEUE_MAX_SOURCES; i++) {
      hdr.set_last_received_offset(i, _header.last_received_offset(i));
    }
    if (write(tmp_fd, &hdr, sizeof(hdr)) != sizeof(hdr)
	|| copy_file_content(fd, _header.begin(), _header.end(), tmp_fd) != 0) {
      goto ERR_OPEN;
    }
    sync_file(tmp_fd);
  }
  /* rename */
  if (rename(tmp_filename, filename) != 0) {
    log("failed to rename (2): %s => %s\n", tmp_filename, filename);
    goto ERR_OPEN;
  }
  // is the directory entry synced with fsync?
  if (fsync(tmp_fd) != 0) {
    kill_proc("failed to sync disk\n");
  }
  /* replace fd */
  close(fd);
  fd = tmp_fd;
  { /* adjust offsets */
    _header.set_begin(_header.begin() - delta);
    _header.set_end(_header.end() - delta);
    _header.set_compaction_offset(_header.compaction_offset() + delta);
    for (queue_rows_owned_t::iterator i = rows_owned.begin();
	 i != rows_owned.end();
	 ++i) {
      i->second -= delta;
    }
    cache.off = 0; /* invalidate, since it may go below sizeof(_header) */
    apply_cond_expr_list(queue_compact_update_cond_expr(delta));
  }
  
  log("finished table compaction: %s\n", table_name);
  return 0;
    
 ERR_OPEN:
  close(tmp_fd);
  unlink(tmp_filename);
 ERR_RETURN:
  unlock();
  return -1;
}

size_t queue_connection_t::cnt = 0;

queue_connection_t *queue_connection_t::current(bool create_if_empty)
{
  queue_connection_t *conn =
    static_cast<queue_connection_t*>(current_thd->ha_data[queue_hton->slot]);
  if (conn == NULL && create_if_empty) {
    conn = new queue_connection_t();
    current_thd->ha_data[queue_hton->slot] = conn;
    cnt++;
  }
  return conn;
}

int queue_connection_t::close(handlerton *hton, THD *thd)
{
  queue_connection_t *conn =
    static_cast<queue_connection_t*>(thd->ha_data[queue_hton->slot]);
  
  if (conn->share_owned != NULL) {
    if (conn->share_owned->reset_owner(pthread_self()) != 0) {
      conn->share_owned->wake_listeners();
    }
    conn->share_owned->release();
  }
  delete conn;
  --cnt;
  
  return 0;
}

void queue_connection_t::erase_owned()
{
  if (share_owned != NULL) {
    share_owned->lock();
    my_off_t off = share_owned->get_owned_row(pthread_self());
    if (off != 0) {
      share_owned->remove_rows(&off, 1);
	share_owned->get_owned_row(pthread_self(), true);
    }
    share_owned->unlock();
    share_owned->release();
    share_owned = NULL;
  }
  owner_mode = false;
}

ha_queue::ha_queue(handlerton *hton, TABLE_SHARE *table_arg)
  :handler(hton, table_arg),
   share(NULL),
   pos(),
   rows(NULL),
   rows_size(0),
   rows_reserved(0),
   bulk_insert_rows(-1),
   bulk_delete_rows(NULL)
{
  assert(ref_length == sizeof(my_off_t));
}

ha_queue::~ha_queue()
{
  delete bulk_delete_rows;
  bulk_delete_rows = NULL;
  free_rows_buffer();
}

static const char *ha_queue_exts[] = {
  Q4M,
  NullS
};

const char **ha_queue::bas_ext() const
{
  return ha_queue_exts;
}

int ha_queue::open(const char *name, int mode, uint test_if_locked)
{
  if ((share = queue_share_t::get_share(name)) == NULL) {
    return 1;
  }
  share->init_fixed_fields(table);
  thr_lock_data_init(share->get_store_lock(), &lock, NULL);
  
  return 0;
}

int ha_queue::close()
{
  share->release();
  
  return 0;
}

int ha_queue::rnd_init(bool scan)
{
  share->lock_reader();
  pos = 0;
  return 0;
}

int ha_queue::rnd_end()
{
  share->unlock_reader();
  free_rows_buffer();
  return 0;
}

int ha_queue::rnd_next(uchar *buf)
{
  assert(rows_size == 0);
  
  int err = HA_ERR_END_OF_FILE;
  share->lock();
  
  queue_connection_t *conn;
  if ((conn = queue_connection_t::current()) != NULL && conn->owner_mode) {
    if (pos == 0 && (pos = share->get_owned_row(pthread_self())) != 0) {
      // ok
    } else {
      goto EXIT;
    }
  } else {
    if (pos == 0) {
      if ((pos = share->header()->begin()) == share->header()->end()) {
	goto EXIT;
      }
    } else {
      if (share->next(&pos) != 0) {
	err = HA_ERR_CRASHED_ON_USAGE;
	goto EXIT;
      } else if (pos == share->header()->end()) {
	goto EXIT;
      }
    }
    while (share->find_owner(pos) != 0) {
      if (share->next(&pos) != 0) {
	err = HA_ERR_CRASHED_ON_USAGE;
	goto EXIT;
      }
      if (pos == share->header()->end()) {
	goto EXIT;
      }
    }
  }
  
  { /* read data to row buffer */
    queue_row_t hdr;
    if (share->read(&hdr, pos, queue_row_t::header_size(), true)
	!= static_cast<ssize_t>(queue_row_t::header_size())) {
      err = HA_ERR_CRASHED_ON_USAGE;
      goto EXIT;
    }
    if (prepare_rows_buffer(queue_row_t::header_size() + hdr.size()) != 0) {
      err = HA_ERR_OUT_OF_MEM;
      goto EXIT;
    }
    if (share->read(rows, pos, queue_row_t::header_size() + hdr.size(), false)
	!= static_cast<ssize_t>(queue_row_t::header_size() + hdr.size())) {
      err = HA_ERR_CRASHED_ON_USAGE;
      goto EXIT;
    }
  }
  
  /* unlock and convert to internal representation */
  share->unlock();
  unpack_row(buf);
  return 0;
  
 EXIT:
  share->unlock();
  return err;
}

void ha_queue::position(const uchar *record)
{
  my_store_ptr(ref, ref_length, pos);
}

int ha_queue::rnd_pos(uchar *buf, uchar *_pos)
{
  assert(rows_size == 0);
  
  pos = my_get_ptr(_pos, ref_length);
  int err = 0;
  
  share->lock();
  /* we should return the row even if it had the deleted flag set during the
   * execution by other threads
   */
  queue_row_t hdr;
  if (share->read(&hdr, pos, queue_row_t::header_size(), true)
      != static_cast<ssize_t>(queue_row_t::header_size())) {
    err = HA_ERR_CRASHED_ON_USAGE;
    goto EXIT;
  }
  if (prepare_rows_buffer(queue_row_t::header_size() + hdr.size()) != 0) {
    err = HA_ERR_OUT_OF_MEM;
    goto EXIT;
  }
  if (share->read(rows, pos, hdr.size(), false)
      != static_cast<ssize_t>(hdr.size())) {
    err = HA_ERR_CRASHED_ON_USAGE;
    goto EXIT;
  }
  share->unlock();
  
  unpack_row(buf);
  return 0;
  
 EXIT:
  share->unlock();
  return err;
}

int ha_queue::info(uint flag)
{
  // records = share->rows.size();
  // deleted = 0;
  
  return 0;
}

THR_LOCK_DATA **ha_queue::store_lock(THD *thd, THR_LOCK_DATA **to,
				     enum thr_lock_type lock_type)
{
  if (lock_type != TL_IGNORE && lock.type == TL_UNLOCK) {
    // just follow ha_archive::store_lock
    if ((TL_WRITE_CONCURRENT_INSERT <= lock_type && lock_type <= TL_WRITE)
	&& ! thd_in_lock_tables(thd) && ! thd_tablespace_op(thd)) {
      lock.type = TL_WRITE_ALLOW_WRITE;
    } else if (lock_type == TL_READ_NO_INSERT && ! thd_in_lock_tables(thd)) {
      lock.type = TL_READ;
    } else {
      lock.type = lock_type;
    }
  }
  
  *to++= &lock;
  return to;
}

int ha_queue::create(const char *name, TABLE *table_arg,
		     HA_CREATE_INFO *create_info)
{
  char filename[FN_REFLEN];
  int fd;
  
  fn_format(filename, name, "", Q4M, MY_REPLACE_EXT | MY_UNPACK_FILENAME);
  if ((fd = ::open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0660))
      == -1) {
    return HA_ERR_GENERIC; // ????
  }
  queue_file_header_t header;
  if (write(fd, &header, sizeof(header)) != sizeof(header)) {
    goto ERROR;
  }
  if (lseek(fd, EXPAND_BY - 1, SEEK_SET) == -1
      || write(fd, "", 1) != 1) {
    goto ERROR;
  }
  sync_file(fd);
  ::close(fd);
  return 0;
  
 ERROR:
  ::close(fd);
  unlink(filename);
  return HA_ERR_RECORD_FILE_FULL;
}

void ha_queue::start_bulk_insert(ha_rows rows __attribute__((unused)))
{
  assert(rows_size == 0);
  assert(bulk_insert_rows == static_cast<size_t>(-1));
  bulk_insert_rows = 0;
}

int ha_queue::end_bulk_insert()
{
  int ret = 0;
  
  if (rows_size != 0) {
    ret = share->write_rows(rows, rows_size);
    switch (ret) {
    case QUEUE_ERR_RECORD_EXISTS:
      ret = 0;
      break;
    }
    rows_size = 0;
  }
  free_rows_buffer();
  bulk_insert_rows = -1;
  
  return ret;
}

bool ha_queue::start_bulk_delete()
{
  assert(bulk_delete_rows == NULL);
  bulk_delete_rows = new vector<my_off_t>();
  return false;
}

int ha_queue::end_bulk_delete()
{
  int ret = 0;
  
  assert(bulk_delete_rows != NULL);
  if (bulk_delete_rows->size() != 0) {
    share->lock();
    ret =
      share->remove_rows(&bulk_delete_rows->front(), bulk_delete_rows->size());
    share->unlock();
  }
  delete bulk_delete_rows;
  bulk_delete_rows = NULL;
  
  return ret;
}

int ha_queue::write_row(uchar *buf)
{
  queue_connection_t *conn = queue_connection_t::current();
  size_t sz;
  
  if (conn != NULL && conn->source.offset() != 0) {
    sz = pack_row(buf, &conn->source);
  } else {
    sz = pack_row(buf, NULL);
  }
  if (sz == 0) {
    return HA_ERR_OUT_OF_MEM;
  }
  if (bulk_insert_rows == static_cast<size_t>(-1)) {
    int err = share->write_rows(rows, sz);
    free_rows_buffer();
    switch (err) {
    case QUEUE_ERR_RECORD_EXISTS:
      err = 0;
      break;
    default:
      return err;
    }
  } else {
    rows_size += sz;
    bulk_insert_rows++;
  }
  
  return 0;
}

int ha_queue::update_row(const uchar *old_data __attribute__((unused)),
			 uchar *new_data)
{
  return HA_ERR_WRONG_COMMAND;
}

int ha_queue::delete_row(const uchar *buf __attribute__((unused)))
{
  int err = 0;
  
  share->lock();
  pthread_t owner = share->find_owner(pos);
  if (owner != 0 && owner != pthread_self()) {
    share->unlock();
    return HA_ERR_RECORD_DELETED;
  }
  if (bulk_delete_rows != NULL) {
    share->unlock();
    bulk_delete_rows->push_back(pos);
  } else {
    err = share->remove_rows(&pos, 1);
    share->unlock();
  }
  
  return err;
}

int ha_queue::prepare_rows_buffer(size_t sz)
{
  if (rows == NULL) {
    assert(rows_size == 0);
    rows_reserved = MIN_ROWS_BUFFER_SIZE;
    while (rows_reserved < sz) {
      rows_reserved *= 2;
    }
    if ((rows = static_cast<uchar*>(my_malloc(rows_reserved, MYF(0))))
	== NULL) {
      return -1;
    }
  } else if (rows_reserved < rows_size + sz) {
    size_t new_reserve = rows_reserved;
    do {
      new_reserve *= 2;
    } while (new_reserve < rows_size + sz);
    void *pt;
    if ((pt = my_realloc(rows, new_reserve, MYF(0))) == NULL) {
      return -1;
    }
    rows = static_cast<uchar*>(pt);
    rows_reserved = new_reserve;
  }
  return 0;
}

void ha_queue::free_rows_buffer()
{
  if (rows != NULL) {
    my_free(rows, MYF(0));
    rows = NULL;
  }
}

void ha_queue::unpack_row(uchar *buf)
{
  const uchar *src = rows + queue_row_t::header_size();
  Field **field;
  queue_fixed_field_t * const * fixed;
  
  memcpy(buf, src, table->s->null_bytes);
  src += table->s->null_bytes;
  for (field = table->field, fixed = share->get_fixed_fields();
       *field != NULL;
       field++, fixed++) {
    if (*fixed != NULL && ! (*field)->is_null()) {
      src = (*field)->unpack(buf + (*field)->offset(table->record[0]), src);
    }
  }
  for (field = table->field, fixed = share->get_fixed_fields();
       *field != NULL;
       field++, fixed++) {
    if (*fixed == NULL && ! (*field)->is_null()) {
      src = (*field)->unpack(buf + (*field)->offset(table->record[0]), src);
    }
  }
}

size_t ha_queue::pack_row(uchar *buf, queue_source_t *source)
{
  /* allocate memory (w. some extra) */
  size_t sz = queue_row_t::header_size() + table->s->reclength
    + table->s->fields * 2;
  if (source != NULL) {
    sz += sizeof(*source);
  }
  for (uint *ptr = table->s->blob_field, *end = ptr + table->s->blob_fields;
       ptr != end;
       ++ptr) {
    sz += 2 + ((Field_blob*)table->field[*ptr])->get_length();
  }
  if (sz > queue_row_t::max_size || prepare_rows_buffer(sz) != 0) {
    return 0;
  }
  /* write data */
  uchar *dst = rows + rows_size + queue_row_t::header_size();
  memcpy(dst, buf, table->s->null_bytes);
  dst += table->s->null_bytes;
  Field **field;
  queue_fixed_field_t * const *fixed;
  for (field = table->field, fixed = share->get_fixed_fields();
       *field != NULL;
       field++, fixed++) {
    if (*fixed != NULL && ! (*field)->is_null()) {
      dst = (*field)->pack(dst, buf + (*field)->offset(buf));
    }
  }
  for (field = table->field, fixed = share->get_fixed_fields();
       *field != NULL;
       field++, fixed++) {
    if (*fixed == NULL && ! (*field)->is_null()) {
      dst = (*field)->pack(dst, buf + (*field)->offset(buf));
    }
  }
  /* write source */
  if (source != NULL) {
    memcpy(dst, source, sizeof(*source));
    dst += sizeof(*source);
  }
  /* write header */
  sz = dst - (rows + rows_size);
  new (reinterpret_cast<queue_row_t*>(rows + rows_size))
    queue_row_t(sz - queue_row_t::header_size(), queue_row_t::type_row);
  return sz;
}

static handler *create_handler(handlerton *hton, TABLE_SHARE *table,
			MEM_ROOT *mem_root)
{
  return new (mem_root) ha_queue(hton, table);
}

static queue_share_t* get_share_check(const char* db_table_name)
{
  char buf[FN_REFLEN];
  char path[FN_REFLEN];
  const char *db, *tbl;
  
  // FIXME: creates bogus name if db_table_name is too long (but no overruns)
  if ((tbl = strchr(db_table_name, '.')) != NULL) {
    size_t db_len = min(static_cast<size_t>(tbl - db_table_name),
			sizeof(buf) - 1);
    memcpy(buf, db_table_name, db_len);
    buf[db_len] = '\0';
    db = buf;
    tbl = tbl + 1;
  } else {
    db = current_thd->db;
    tbl = db_table_name;
  }
  if (db == NULL) {
    return NULL;
  }
  
  build_table_filename(path, FN_REFLEN - 1, db, tbl, "", 0);
  queue_share_t *share = queue_share_t::get_share(path);
  if (! share->init_fixed_fields(NULL)) {
    log("failed to initialize fixed field info.\n");
    share->release();
    share = NULL;
  }
  return share;
}

static int init_plugin(void *p)
{
  queue_hton = (handlerton *)p;
  
  pthread_mutex_init(&open_mutex, MY_MUTEX_INIT_FAST);
  pthread_mutex_init(&listener_mutex, MY_MUTEX_INIT_FAST);
  hash_init(&queue_open_tables, system_charset_info, 32, 0, 0,
	    reinterpret_cast<hash_get_key>(queue_share_t::get_share_key), 0, 0);
  queue_hton->state = SHOW_OPTION_YES;
  queue_hton->close_connection = queue_connection_t::close;
  queue_hton->create = create_handler;
  queue_hton->flags = HTON_CAN_RECREATE;
  
  return 0;
}

static int deinit_plugin(void *p)
{
  if (queue_connection_t::cnt != 0) {
    // FIXME: what is the appropriate error code to return busy status
    return HA_ERR_GENERIC;
  }
  
  hash_free(&queue_open_tables);
  pthread_mutex_destroy(&listener_mutex);
  pthread_mutex_destroy(&open_mutex);
  queue_hton = NULL;
  
  return 0;
}

struct st_mysql_storage_engine queue_storage_engine = {
  MYSQL_HANDLERTON_INTERFACE_VERSION
};

mysql_declare_plugin(queue)
{
  MYSQL_STORAGE_ENGINE_PLUGIN,
  &queue_storage_engine,
  "QUEUE",
  "Kazuho Oku at Cybozu Labs, Inc.",
  "Queue storage engine for MySQL",
  PLUGIN_LICENSE_GPL,
  init_plugin,
  deinit_plugin,
  0x0001,
  NULL,                       /* status variables                */
  NULL,                       /* system variables                */
  NULL                        /* config options                  */
}
mysql_declare_plugin_end;

static int _queue_wait_core(char **share_names, int num_shares, int timeout,
			    char *error)
{
  queue_share_t **shares;
  queue_share_t::cond_expr_t **cond_exprs;
  int share_owned = -1;
  queue_connection_t *conn = queue_connection_t::current(true);
  
  *error = 0;
  
  conn->erase_owned();
  
  /* setup */
  if (my_multi_malloc(MY_ZEROFILL, &shares, num_shares * sizeof(queue_share_t*),
		      &cond_exprs,
		      num_shares * sizeof(queue_share_t::cond_expr_t*), NullS)
      == NULL) {
    log("out of memory\n");
    *error = 1;
    return 0;
  }
  
  /* setup, or immediately break if data found, note that locks for the tables
   * are not released until all the tables are scanned, in order NOT to return
   * a row of a lower-priority table */
  for (int i = 0; i < num_shares; i++) {
    const char *last = strchr(share_names[i], ':');
    if (last == NULL) {
      last = share_names[i] + strlen(share_names[i]);
    }
    if (last - share_names[i] > FN_REFLEN * 2) {
      log("table name too long: %s\n", share_names[i]);
      *error = 1;
      break;
    }
    char db_table_name[FN_REFLEN * 2 + 1];
    memcpy(db_table_name, share_names[i], last - share_names[i]);
    db_table_name[last - share_names[i]] = '\0';
    if ((shares[i] = get_share_check(db_table_name)) == NULL) {
      log("could not find table: %s\n", db_table_name);
      *error = 1;
      break;
    }
    shares[i]->lock();
    if ((cond_exprs[i] =
	 *last != '\0'
	 ? shares[i]->compile_cond_expr(last + 1, strlen(last + 1))
	 : shares[i]->compile_cond_expr(NULL, 0))
	== NULL) {
      log("failed to compile expression: %s\n", share_names[i]);
      *error = 1;
      break;
    }
    if (shares[i]->assign_owner(pthread_self(), cond_exprs[i]) != 0) {
      share_owned = i;
      break;
    }
  }
  for (int i = 0; i < num_shares && shares[i] != NULL; i++) {
    shares[i]->unlock();
  }
  if (*error != 0) {
    goto EXIT;
  }
  if (share_owned == -1) {
    /* not yet found, lock global mutex and check once more */
    pthread_mutex_lock(&listener_mutex);
    for (int i = 0; i < num_shares; i++) {
      shares[i]->lock();
      if (shares[i]->assign_owner(pthread_self(), cond_exprs[i]) != 0) {
	for (int j = 0; j <= i; j++) {
	  shares[j]->unlock();
	}
	share_owned = i;
	break;
      }
    }
    /* if not yet found, wait for data */
    if (share_owned == -1) {
      queue_share_t::listener_t listener(pthread_self());
      for (int i = 0; i < num_shares; i++) {
	shares[i]->register_listener(&listener, cond_exprs[i]);
	shares[i]->unlock();
      }
#ifdef USE_RELATIVE_TIMEDWAIT
      timespec ts = { timeout, 0 };
      pthread_cond_timedwait_relative_np(&listener.cond, &listener_mutex, &ts);
#else
      timespec ts = { time(NULL) + timeout, 0 };
      pthread_cond_timedwait(&listener.cond, &listener_mutex, &ts);
#endif
      for (int i = 0; i < num_shares; i++) {
	if (listener.signalled_by == shares[i]) {
	  share_owned = i;
	} else {
	  shares[i]->unregister_listener(&listener);
	}
      }
    }
    pthread_mutex_unlock(&listener_mutex);
  }
  /* always enter owner-mode, regardless whether or not we own a row */
  conn->owner_mode = true;
  conn->share_owned = share_owned != -1 ? shares[share_owned] : NULL;
  
 EXIT:
  for (int i = 0; i < num_shares && cond_exprs[i] != NULL; i++) {
    shares[i]->release_cond_expr(cond_exprs[i]);
  }
  for (int i = 0; i < num_shares && shares[i] != NULL; i++) {
    if (i != share_owned) {
      shares[i]->release();
    }
  }
  my_free(shares, MYF(0));
  return share_owned;
}

my_bool queue_wait_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
  if (args->arg_count == 0) {
    strcpy(message, "queue_wait(table_name[,timeout]): argument error");
    return 1;
  } else if (args->arg_count >= 2) {
    args->arg_type[args->arg_count - 1] = INT_RESULT;
    args->maybe_null[args->arg_count - 1] = 0;
  }
  for (int i = max(args->arg_count - 2, 0); i >= 0; i--) {
    args->arg_type[i] = STRING_RESULT;
    args->maybe_null[i] = 0;
  }
  initid->maybe_null = 0;
  
  return 0;
}

void queue_wait_deinit(UDF_INIT *initid __attribute__((unused)))
{
}

long long queue_wait(UDF_INIT *initid __attribute__((unused)), UDF_ARGS *args,
		     char *is_null, char *error)
{
  int timeout = args->arg_count >= 2
    ? *reinterpret_cast<long long*>(args->args[args->arg_count - 1]) : 60;
  
  *is_null = 0;
  return
    _queue_wait_core(args->args, max(args->arg_count - 1, 1), timeout, error)
    + 1;
}

my_bool queue_end_init(UDF_INIT *initid,
		       UDF_ARGS *args __attribute__((unused)),
		       char *message __attribute__((unused)))
{
  initid->maybe_null = 0;
  return 0;
}

void queue_end_deinit(UDF_INIT *initid __attribute__((unused)))
{
}

long long queue_end(UDF_INIT *initid __attribute__((unused)),
		    UDF_ARGS *args __attribute__((unused)),
		    char *is_null, char *error __attribute__((unused)))
{
  queue_connection_t *conn;
  
  if ((conn = queue_connection_t::current()) != NULL) {
    conn->erase_owned();
  }
  
  *is_null = 0;
  return 1;
}

my_bool queue_abort_init(UDF_INIT *initid,
			UDF_ARGS *args __attribute__((unused)),
			char *message)
{
  queue_connection_t *conn;
  if ((conn = queue_connection_t::current()) == NULL || ! conn->owner_mode) {
    strcpy(message, "queue_abort(): not in owner mode");
    return 1;
  }
  initid->maybe_null = 0;
  return 0;
}

void queue_abort_deinit(UDF_INIT *initid)
{
}

long long queue_abort(UDF_INIT *initid, UDF_ARGS *args __attribute__((unused)),
		      char *is_null, char *error)
{
  queue_connection_t *conn;
  
  if ((conn = queue_connection_t::current()) != NULL) {
    if (conn->share_owned != NULL) {
      if (conn->share_owned->reset_owner(pthread_self()) != 0) {
	conn->share_owned->wake_listeners();
      }
      conn->share_owned->release();
      conn->share_owned = NULL;
    }
    conn->owner_mode = false;
  }
  
  *is_null = 0;
  return 1;
}

my_bool queue_rowid_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
  if (args->arg_count != 0) {
    strcpy(message, "queue_rowid(): argument error");
    return 1;
  }
  queue_connection_t *conn;
  if ((conn = queue_connection_t::current()) == NULL || ! conn->owner_mode) {
    strcpy(message, "queue_rowid(): not in owner mode");
    return 1;
  }
  initid->maybe_null = 1;
  return 0;
}

void queue_rowid_deinit(UDF_INIT *initid __attribute__((unused)))
{
}

long long queue_rowid(UDF_INIT *initid __attribute__((unused)), UDF_ARGS *args,
		      char *is_null, char *error)
{
  queue_connection_t *conn;
  if ((conn = queue_connection_t::current()) == NULL) {
    log("internal error, unexpectedly conn==NULL\n");
    *error = 1;
    return 0;
  }
  queue_share_t *share;
  if (! conn->owner_mode || (share = conn->share_owned) == NULL) {
    *is_null = 1;
    return 0;
  }
  share->lock();
  my_off_t off = share->get_owned_row(pthread_self())
    + share->header()->compaction_offset();
  share->unlock();
  return static_cast<long long>(off);
}

my_bool queue_set_srcid_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
  if (args->arg_count != 3) {
    strcpy(message, "queue_set_srcid(source,mode,rowid): argument error");
    return 1;
  }
  args->arg_type[0] = INT_RESULT;
  args->maybe_null[0] = 0;
  args->arg_type[1] = STRING_RESULT;
  args->maybe_null[1] = 0;
  args->arg_type[2] = INT_RESULT;
  args->maybe_null[2] = 0;
  initid->maybe_null = 0;
  return 0;
}

void queue_set_srcid_deinit(UDF_INIT *initid __attribute__((unused)))
{
}

long long queue_set_srcid(UDF_INIT *initid __attribute__((unused)),
			  UDF_ARGS *args, char *is_null __attribute__((unused)),
			  char *error)
{
  long long sender = *(long long*)args->args[0];
  if (sender < 0 || QUEUE_MAX_SOURCES <= sender) {
    log("queue_set_srcid: source number exceeds limit: %lld\n", sender);
    *error = 1;
    return 0;
  }
  queue_connection_t *conn = queue_connection_t::current(true);
  if (strcmp(args->args[1], "a") == 0) {
    conn->reset_source = false;
  } else if (strcmp(args->args[1], "w") == 0) {
    conn->reset_source = true;
  } else {
    log("queue_set_srcid: invalid mode: %s\n", args->args[1]);
    *error = 1;
    return 0;
  }
  conn->source = queue_source_t(sender, *(long long*)args->args[2]);
  return 1;
}

#if defined(USE_RELATIVE_TIMEDWAIT) && defined(SAFE_MUTEX)
#undef pthread_mutex_lock
#undef pthread_mutex_unlock
#undef pthread_cond_timedwait_relative_np
int safe_cond_timedwait_relative_np(pthread_cond_t *cond, safe_mutex_t *mp,
				    struct timespec *abstime, const char *file,
				    uint line)
{
  int error;
  pthread_mutex_lock(&mp->global);
  if (mp->count != 1 || !pthread_equal(pthread_self(),mp->thread))
  {
    fprintf(stderr,"safe_mutex: Trying to cond_wait at %s, line %d on a not hold mutex\n",file,line);
    fflush(stderr);
    abort();
  }
  mp->count--;                                  /* Mutex will be released */
  pthread_mutex_unlock(&mp->global);
  error=pthread_cond_timedwait_relative_np(cond,&mp->mutex,abstime);
#ifdef EXTRA_DEBUG
  if (error && (error != EINTR && error != ETIMEDOUT && error != ETIME))
  {
    fprintf(stderr,"safe_mutex: Got error: %d (%d) when doing a safe_mutex_timedwait at %s, line %d\n", error, errno, file, line);
  }
#endif
  pthread_mutex_lock(&mp->global);
  mp->thread=pthread_self();
  if (mp->count++)
  {
    fprintf(stderr,
            "safe_mutex:  Count was %d in thread 0x%lx when locking mutex at %s, line %d (error: %d (%d))\n",
            mp->count-1, my_thread_dbug_id(), file, line, error, error);
    fflush(stderr);
    abort();
  }
  mp->file= file;
  mp->line=line;
  pthread_mutex_unlock(&mp->global);
  return error;
}
#endif
