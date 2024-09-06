/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "server.h"

/*-----------------------------------------------------------------------------
 * List API
 *----------------------------------------------------------------------------*/

/* Check the length and size of a number of objects that will be added to list to see
 * if we need to convert a listpack to a quicklist. Note that we only check string
 * encoded objects as their string length can be queried in constant time.
 *
 * If callback is given the function is called in order for caller to do some work
 * before the list conversion. */
static void listTypeTryConvertListpack(robj *o, robj **argv, int start, int end,
                                       beforeConvertCB fn, void *data)
{
    serverAssert(o->encoding == OBJ_ENCODING_LISTPACK);

    size_t add_bytes = 0;
    size_t add_length = 0;

    if (argv) {
        for (int i = start; i <= end; i++) {
            if (!sdsEncodedObject(argv[i]))
                continue;
            add_bytes += sdslen(argv[i]->ptr);
        }
        add_length = end - start + 1;
    }

    if (quicklistNodeExceedsLimit(server.list_max_listpack_size,
            lpBytes(o->ptr) + add_bytes, lpLength(o->ptr) + add_length))
    {
        /* Invoke callback before conversion. */
        if (fn) fn(data);

        quicklist *ql = quicklistCreate();
        quicklistSetOptions(ql, server.list_max_listpack_size, server.list_compress_depth);

        /* Append listpack to quicklist if it's not empty, otherwise release it. */
        if (lpLength(o->ptr))
            quicklistAppendListpack(ql, o->ptr);
        else
            lpFree(o->ptr);
        o->ptr = ql;
        o->encoding = OBJ_ENCODING_QUICKLIST;
    }
}

/* Check the length and size of a quicklist to see if we need to convert it to listpack.
 *
 * 'shrinking' is 1 means that the conversion is due to a list shrinking, to avoid
 * frequent conversions of quicklist and listpack due to frequent insertion and
 * deletion, we don't convert quicklist to listpack until its length or size is
 * below half of the limit.
 *
 * If callback is given the function is called in order for caller to do some work
 * before the list conversion. */
static void listTypeTryConvertQuicklist(robj *o, int shrinking, beforeConvertCB fn, void *data) {
    serverAssert(o->encoding == OBJ_ENCODING_QUICKLIST);

    size_t sz_limit;
    unsigned int count_limit;
    quicklist *ql = o->ptr;

    /* A quicklist can be converted to listpack only if it has only one packed node. */
    if (ql->len != 1 || ql->head->container != QUICKLIST_NODE_CONTAINER_PACKED)
        return;

    /* Check the length or size of the quicklist is below the limit. */
    quicklistNodeLimit(server.list_max_listpack_size, &sz_limit, &count_limit);
    if (shrinking) {
        sz_limit /= 2;
        count_limit /= 2;
    }
    if (ql->head->sz > sz_limit || ql->count > count_limit) return;

    /* Invoke callback before conversion. */
    if (fn) fn(data);

    /* Extract the listpack from the unique quicklist node,
     * then reset it and release the quicklist. */
    o->ptr = ql->head->entry;
    ql->head->entry = NULL;
    quicklistRelease(ql);
    o->encoding = OBJ_ENCODING_LISTPACK;
}

/* Check if the list needs to be converted to appropriate encoding due to
 * growing, shrinking or other cases.
 *
 * 'lct' can be one of the following values:
 * LIST_CONV_AUTO      - Used after we built a new list, and we want to let the
 *                       function decide on the best encoding for that list.
 * LIST_CONV_GROWING   - Used before or right after adding elements to the list,
 *                       in which case we are likely to only consider converting
 *                       from listpack to quicklist.
 *                       'argv' is only used in this case to calculate the size
 *                       of a number of objects that will be added to list.
 * LIST_CONV_SHRINKING - Used after removing an element from the list, in which case we
 *                       wanna consider converting from quicklist to listpack. When we
 *                       know we're shrinking, we use a lower (more strict) threshold in
 *                       order to avoid repeated conversions on every list change. */
static void listTypeTryConversionRaw(robj *o, list_conv_type lct,
                                     robj **argv, int start, int end,
                                     beforeConvertCB fn, void *data)
{
    if (o->encoding == OBJ_ENCODING_QUICKLIST) {
        if (lct == LIST_CONV_GROWING) return; /* Growing has nothing to do with quicklist */
        listTypeTryConvertQuicklist(o, lct == LIST_CONV_SHRINKING, fn, data);
    } else if (o->encoding == OBJ_ENCODING_LISTPACK) {
        if (lct == LIST_CONV_SHRINKING) return; /* Shrinking has nothing to do with listpack */
        listTypeTryConvertListpack(o, argv, start, end, fn, data);
    } else {
        serverPanic("Unknown list encoding");
    }
}

/* This is just a wrapper for listTypeTryConversionRaw() that is
 * able to try conversion without passing 'argv'. */
void listTypeTryConversion(robj *o, list_conv_type lct, beforeConvertCB fn, void *data) {
    listTypeTryConversionRaw(o, lct, NULL, 0, 0, fn, data);
}

/* This is just a wrapper for listTypeTryConversionRaw() that is
 * able to try conversion before adding elements to the list. */
void listTypeTryConversionAppend(robj *o, robj **argv, int start, int end,
                                 beforeConvertCB fn, void *data)
{
    listTypeTryConversionRaw(o, LIST_CONV_GROWING, argv, start, end, fn, data);
}

/* The function pushes an element to the specified list object 'subject',
 * at head or tail position as specified by 'where'.
 *
 * There is no need for the caller to increment the refcount of 'value' as
 * the function takes care of it if needed. */
void listTypePush(robj *subject, robj *value, int where) {
    if (subject->encoding == OBJ_ENCODING_QUICKLIST) {
        int pos = (where == LIST_HEAD) ? QUICKLIST_HEAD : QUICKLIST_TAIL;
        if (value->encoding == OBJ_ENCODING_INT) {
            char buf[32];
            ll2string(buf, 32, (long)value->ptr);
            quicklistPush(subject->ptr, buf, strlen(buf), pos);
        } else {
            quicklistPush(subject->ptr, value->ptr, sdslen(value->ptr), pos);
        }
    } else if (subject->encoding == OBJ_ENCODING_LISTPACK) {
        if (value->encoding == OBJ_ENCODING_INT) {
            subject->ptr = (where == LIST_HEAD) ?
                lpPrependInteger(subject->ptr, (long)value->ptr) :
                lpAppendInteger(subject->ptr, (long)value->ptr);
        } else {
            subject->ptr = (where == LIST_HEAD) ?
                lpPrepend(subject->ptr, value->ptr, sdslen(value->ptr)) :
                lpAppend(subject->ptr, value->ptr, sdslen(value->ptr));
        }
    } else {
        serverPanic("Unknown list encoding");
    }
}

void *listPopSaver(unsigned char *data, size_t sz) {
    return createStringObject((char*)data,sz);
}

robj *listTypePop(robj *subject, int where) {
    robj *value = NULL;

    if (subject->encoding == OBJ_ENCODING_QUICKLIST) {
        long long vlong;
        int ql_where = where == LIST_HEAD ? QUICKLIST_HEAD : QUICKLIST_TAIL;
        if (quicklistPopCustom(subject->ptr, ql_where, (unsigned char **)&value,
                               NULL, &vlong, listPopSaver)) {
            if (!value)
                value = createStringObjectFromLongLong(vlong);
        }
    } else if (subject->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *p;
        unsigned char *vstr;
        int64_t vlen;
        unsigned char intbuf[LP_INTBUF_SIZE];

        p = (where == LIST_HEAD) ? lpFirst(subject->ptr) : lpLast(subject->ptr);
        if (p) {
            vstr = lpGet(p, &vlen, intbuf);
            value = createStringObject((char*)vstr, vlen);
            subject->ptr = lpDelete(subject->ptr, p, NULL);
        }
    } else {
        serverPanic("Unknown list encoding");
    }
    return value;
}

unsigned long listTypeLength(const robj *subject) {
    if (subject->encoding == OBJ_ENCODING_QUICKLIST) {
        return quicklistCount(subject->ptr);
    } else if (subject->encoding == OBJ_ENCODING_LISTPACK) {
        return lpLength(subject->ptr);
    } else {
        serverPanic("Unknown list encoding");
    }
}

/* Initialize an iterator at the specified index. */
listTypeIterator *listTypeInitIterator(robj *subject, long index,
                                       unsigned char direction) {
    listTypeIterator *li = zmalloc(sizeof(listTypeIterator));
    li->subject = subject;
    li->encoding = subject->encoding;
    li->direction = direction;
    li->iter = NULL;
    /* LIST_HEAD means start at TAIL and move *towards* head.
     * LIST_TAIL means start at HEAD and move *towards* tail. */
    if (li->encoding == OBJ_ENCODING_QUICKLIST) {
        int iter_direction = direction == LIST_HEAD ? AL_START_TAIL : AL_START_HEAD;
        li->iter = quicklistGetIteratorAtIdx(li->subject->ptr,
                                             iter_direction, index);
    } else if (li->encoding == OBJ_ENCODING_LISTPACK) {
        li->lpi = lpSeek(subject->ptr, index);
    } else {
        serverPanic("Unknown list encoding");
    }
    return li;
}

/* Sets the direction of an iterator. */
void listTypeSetIteratorDirection(listTypeIterator *li, listTypeEntry *entry, unsigned char direction) {
    if (li->direction == direction) return;

    li->direction = direction;
    if (li->encoding == OBJ_ENCODING_QUICKLIST) {
        int dir = direction == LIST_HEAD ? AL_START_TAIL : AL_START_HEAD;
        quicklistSetDirection(li->iter, dir);
    } else if (li->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *lp = li->subject->ptr;
        /* Note that the iterator for listpack always points to the next of the current entry,
         * so we need to update position of the iterator depending on the direction. */
        li->lpi = (direction == LIST_TAIL) ? lpNext(lp, entry->lpe) : lpPrev(lp, entry->lpe);
    } else {
        serverPanic("Unknown list encoding");
    }
}

/* Clean up the iterator. */
void listTypeReleaseIterator(listTypeIterator *li) {
    if (li->encoding == OBJ_ENCODING_QUICKLIST)
        quicklistReleaseIterator(li->iter);
    zfree(li);
}

/* Stores pointer to current the entry in the provided entry structure
 * and advances the position of the iterator. Returns 1 when the current
 * entry is in fact an entry, 0 otherwise. */
int listTypeNext(listTypeIterator *li, listTypeEntry *entry) {
    /* Protect from converting when iterating */
    serverAssert(li->subject->encoding == li->encoding);

    entry->li = li;
    if (li->encoding == OBJ_ENCODING_QUICKLIST) {
        return quicklistNext(li->iter, &entry->entry);
    } else if (li->encoding == OBJ_ENCODING_LISTPACK) {
        entry->lpe = li->lpi;
        if (entry->lpe != NULL) {
            li->lpi = (li->direction == LIST_TAIL) ?
                lpNext(li->subject->ptr,li->lpi) : lpPrev(li->subject->ptr,li->lpi);
            return 1;
        }
    } else {
        serverPanic("Unknown list encoding");
    }
    return 0;
}

/* Get entry value at the current position of the iterator.
 * When the function returns NULL, it populates the integer value by
 * reference in 'lval'. Otherwise a pointer to the string is returned,
 * and 'vlen' is set to the length of the string. */
unsigned char *listTypeGetValue(listTypeEntry *entry, size_t *vlen, long long *lval) {
    unsigned char *vstr = NULL;
    if (entry->li->encoding == OBJ_ENCODING_QUICKLIST) {
        if (entry->entry.value) {
            vstr = entry->entry.value;
            *vlen = entry->entry.sz;
        } else {
            *lval = entry->entry.longval;
        }
    } else if (entry->li->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned int slen;
        vstr = lpGetValue(entry->lpe, &slen, lval);
        *vlen = slen;
    } else {
        serverPanic("Unknown list encoding");
    }
    return vstr;
}

/* Return entry or NULL at the current position of the iterator. */
robj *listTypeGet(listTypeEntry *entry) {
    unsigned char *vstr;
    size_t vlen;
    long long lval;

    vstr = listTypeGetValue(entry, &vlen, &lval);
    if (vstr) 
        return createStringObject((char *)vstr, vlen);
    else
        return createStringObjectFromLongLong(lval);
}

void listTypeInsert(listTypeEntry *entry, robj *value, int where) {
    robj *subject = entry->li->subject;
    value = getDecodedObject(value);
    sds str = value->ptr;
    size_t len = sdslen(str);

    if (entry->li->encoding == OBJ_ENCODING_QUICKLIST) {
        if (where == LIST_TAIL) {
            quicklistInsertAfter(entry->li->iter, &entry->entry, str, len);
        } else if (where == LIST_HEAD) {
            quicklistInsertBefore(entry->li->iter, &entry->entry, str, len);
        }
    } else if (entry->li->encoding == OBJ_ENCODING_LISTPACK) {
        int lpw = (where == LIST_TAIL) ? LP_AFTER : LP_BEFORE;
        subject->ptr = lpInsertString(subject->ptr, (unsigned char *)str,
                                      len, entry->lpe, lpw, &entry->lpe);
    } else {
        serverPanic("Unknown list encoding");
    }
    decrRefCount(value);
}

/* Replaces entry at the current position of the iterator. */
void listTypeReplace(listTypeEntry *entry, robj *value) {
    robj *subject = entry->li->subject;
    value = getDecodedObject(value);
    sds str = value->ptr;
    size_t len = sdslen(str);

    if (entry->li->encoding == OBJ_ENCODING_QUICKLIST) {
        quicklistReplaceEntry(entry->li->iter, &entry->entry, str, len);
    } else if (entry->li->encoding == OBJ_ENCODING_LISTPACK) {
        subject->ptr = lpReplace(subject->ptr, &entry->lpe, (unsigned char *)str, len);
    } else {
        serverPanic("Unknown list encoding");
    }

    decrRefCount(value);
}

/* Delete the element pointed to. */
void listTypeDelete(listTypeIterator *iter, listTypeEntry *entry) {
    if (entry->li->encoding == OBJ_ENCODING_QUICKLIST) {
        quicklistDelEntry(iter->iter, &entry->entry);
    } else if (entry->li->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *p = entry->lpe;
        iter->subject->ptr = lpDelete(iter->subject->ptr,p,&p);

        /* Update position of the iterator depending on the direction */
        if (iter->direction == LIST_TAIL)
            iter->lpi = p;
        else {
            if (p) {
                iter->lpi = lpPrev(iter->subject->ptr,p);
            } else {
                /* We deleted the last element, so we need to set the
                 * iterator to the last element. */
                iter->lpi = lpLast(iter->subject->ptr);
            }
        }
    } else {
        serverPanic("Unknown list encoding");
    }
}

/*-----------------------------------------------------------------------------
 * List Commands
 *----------------------------------------------------------------------------*/

/* Implements LPUSH/RPUSH/LPUSHX/RPUSHX. 
 * 'xx': push if key exists. */
void pushGenericCommand(client *c, int where, int xx) {
    int j;

    robj *lobj = lookupKeyWrite(c->db, c->argv[1]);
    if (checkType(c,lobj,OBJ_LIST)) return;
    if (!lobj) {
        if (xx) {
            addReply(c, shared.czero);
            return;
        }

        lobj = createListListpackObject();
        dbAdd(c->db,c->argv[1],lobj);
    }

    listTypeTryConversionAppend(lobj,c->argv,2,c->argc-1,NULL,NULL);
    for (j = 2; j < c->argc; j++) {
        listTypePush(lobj,c->argv[j],where);
        server.dirty++;
    }

    addReplyLongLong(c, listTypeLength(lobj));

    char *event = (where == LIST_HEAD) ? "lpush" : "rpush";
    signalModifiedKey(c,c->db,c->argv[1]);
    notifyKeyspaceEvent(NOTIFY_LIST,event,c->argv[1],c->db->id);
}

/* RPUSH <key> <element> [<element> ...] */
void rpushCommand(client *c) {
    pushGenericCommand(c,LIST_TAIL,0);
}
