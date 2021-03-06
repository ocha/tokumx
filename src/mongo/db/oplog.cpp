// @file oplog.cpp

/**
*    Copyright (C) 2008 10gen Inc.
*    Copyright (C) 2013 Tokutek Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "mongo/pch.h"

#include <vector>

#include "mongo/base/counter.h"
#include "mongo/db/oplog.h"
#include "mongo/db/cmdline.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/repl_block.h"
#include "mongo/db/repl.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/server_status.h"
#include "mongo/db/repl/rs.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/stats/timer_stats.h"
#include "mongo/db/query_optimizer_internal.h"
#include "mongo/db/collection.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/instance.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/oplog_helpers.h"
#include "mongo/db/jsobjmanipulator.h"
#include "mongo/util/elapsed_tracker.h"

namespace mongo {

    // cached copies of these...so don't rename them, drop them, etc.!!!
    static Collection *rsOplogDetails = NULL;
    static Collection *rsOplogRefsDetails = NULL;
    static Collection *replInfoDetails = NULL;
    
    void deleteOplogFiles() {
        rsOplogDetails = NULL;
        rsOplogRefsDetails = NULL;
        replInfoDetails = NULL;
        
        Client::Context ctx(rsoplog, dbpath);
        // TODO: code review this for possible error cases
        // although, I don't think we care about error cases,
        // just that after we exit, oplog files don't exist
        Collection *cl;
        BSONObjBuilder out;
        string errmsg;

        cl = getCollection(rsoplog);
        if (cl != NULL) {
            cl->drop(errmsg, out);
        }
        cl = getCollection(rsOplogRefs);
        if (cl != NULL) {
            cl->drop(errmsg, out);
        }
        cl = getCollection(rsReplInfo);
        if (cl != NULL) {
            cl->drop(errmsg, out);
        }
    }

    bool oplogFilesOpen() {
        return (rsOplogDetails != NULL && rsOplogRefsDetails != NULL && replInfoDetails != NULL);
    }

    void openOplogFiles() {
        const char *logns = rsoplog;
        if (rsOplogDetails == NULL) {
            Client::Context ctx(logns , dbpath);
            rsOplogDetails = getCollection(logns);
            massert(13347, "local.oplog.rs missing. did you drop it? if so restart server", rsOplogDetails);
        }
        if (rsOplogRefsDetails == NULL) {
            Client::Context ctx(rsOplogRefs , dbpath);
            rsOplogRefsDetails = getCollection(rsOplogRefs);
            massert(16814, "local.oplog.refs missing. did you drop it? if so restart server", rsOplogRefsDetails);
        }
        if (replInfoDetails == NULL) {
            Client::Context ctx(rsReplInfo , dbpath);
            replInfoDetails = getCollection(rsReplInfo);
            massert(16747, "local.replInfo missing. did you drop it? if so restart server", replInfoDetails);
        }
    }
    
    static void _logTransactionOps(GTID gtid, uint64_t timestamp, uint64_t hash, BSONArray& opInfo) {
        LOCK_REASON(lockReason, "repl: logging to oplog");
        Lock::DBRead lk1("local", lockReason);

        BSONObjBuilder b;
        addGTIDToBSON("_id", gtid, b);
        b.appendDate("ts", timestamp);
        b.append("h", (long long)hash);
        b.append("a", true);
        b.append("ops", opInfo);

        BSONObj bb = b.done();
        // write it to oplog
        LOG(3) << "writing " << bb.toString(false, true) << " to master " << endl;
        writeEntryToOplog(bb, true);
    }

    // assumes it is locked on entry
    void logToReplInfo(GTID minLiveGTID, GTID minUnappliedGTID) {
        BufBuilder bufbuilder(256);
        BSONObjBuilder b(bufbuilder);
        b.append("_id", "minLive");
        addGTIDToBSON("GTID", minLiveGTID, b);
        BSONObj bb = b.done();
        const uint64_t flags = Collection::NO_UNIQUE_CHECKS | Collection::NO_LOCKTREE;
        replInfoDetails->insertObject(bb, flags);

        bufbuilder.reset();
        BSONObjBuilder b2(bufbuilder);
        b2.append("_id", "minUnapplied");
        addGTIDToBSON("GTID", minUnappliedGTID, b2);
        BSONObj bb2 = b2.done();
        replInfoDetails->insertObject(bb2, flags);
    }
    
    void logTransactionOps(GTID gtid, uint64_t timestamp, uint64_t hash, BSONArray& opInfo) {
        _logTransactionOps(gtid, timestamp, hash, opInfo);
    }

    void logTransactionOpsRef(GTID gtid, uint64_t timestamp, uint64_t hash, OID& oid) {
        LOCK_REASON(lockReason, "repl: logging to oplog");
        Lock::DBRead lk1("local", lockReason);
        BSONObjBuilder b;
        addGTIDToBSON("_id", gtid, b);
        b.appendDate("ts", timestamp);
        b.append("h", (long long)hash);
        b.append("a", true);
        b.append("ref", oid);
        BSONObj bb = b.done();
        writeEntryToOplog(bb, true);
    }

    void logOpsToOplogRef(BSONObj o) {
        LOCK_REASON(lockReason, "repl: logging to oplog.refs");
        Lock::DBRead lk("local", lockReason);
        writeEntryToOplogRefs(o);
    }

    void createOplog() {
        bool rs = !cmdLine._replSet.empty();
        verify(rs);
        
        const char * oplogNS = rsoplog;
        const char * replInfoNS = rsReplInfo;
        Client::Context ctx(oplogNS);
        Collection * oplogNSD = getCollection(oplogNS);
        Collection * oplogRefsNSD = getCollection(rsOplogRefs);        
        Collection * replInfoNSD = getCollection(replInfoNS);
        if (oplogNSD || replInfoNSD || oplogRefsNSD) {
            // TODO: (Zardosht), figure out if there are any checks to do here
            // not sure under what scenarios we can be here, so
            // making a printf to catch this so we can investigate
            tokulog() << "createOplog called with existing collections, investigate why.\n" << endl;
            return;
        }

        /* create an oplog collection, if it doesn't yet exist. */
        BSONObjBuilder b;

        // create the namespace
        string err;
        BSONObj o = b.done();
        bool ret;
        ret = userCreateNS(oplogNS, o, err, false);
        verify(ret);
        ret = userCreateNS(rsOplogRefs, o, err, false);
        verify(ret);
        ret = userCreateNS(replInfoNS, o, err, false);
        verify(ret);
    }

    GTID getGTIDFromOplogEntry(BSONObj o) {
        return getGTIDFromBSON("_id", o);
    }

    bool getLastGTIDinOplog(GTID* gtid) {
        LOCK_REASON(lockReason, "repl: looking up last GTID in oplog");
        Client::ReadContext ctx(rsoplog, lockReason);
        // TODO: Should this be using rsOplogDetails, verifying non-null?
        Collection *cl = getCollection(rsoplog);
        shared_ptr<Cursor> c( Cursor::make(cl, -1) );
        if (c->ok()) {
            *gtid = getGTIDFromOplogEntry(c->current());
            return true;
        }
        return false;
    }

    bool gtidExistsInOplog(GTID gtid) {
        LOCK_REASON(lockReason, "repl: querying for GTID in oplog");
        Client::ReadContext ctx(rsoplog, lockReason);
        BSONObjBuilder q;
        BSONObj result;
        addGTIDToBSON("_id", gtid, q);
        const bool found = Collection::findOne(rsoplog, q.done(), result);
        return found;
    }

    static void _writeEntryToOplog(BSONObj entry) {
        verify(rsOplogDetails);

        const uint64_t flags = Collection::NO_UNIQUE_CHECKS | Collection::NO_LOCKTREE;
        rsOplogDetails->insertObject(entry, flags);
    }

    void writeEntryToOplog(BSONObj entry, bool recordStats) {
        if (recordStats) {
            TimerHolder insertTimer(&oplogInsertStats);
            oplogInsertBytesStats.increment(entry.objsize());
            _writeEntryToOplog(entry);
        } else {
            _writeEntryToOplog(entry);
        }
    }

    void writeEntryToOplogRefs(BSONObj o) {
        verify(rsOplogRefsDetails);

        TimerHolder insertTimer(&oplogInsertStats);
        oplogInsertBytesStats.increment(o.objsize());

        const uint64_t flags = Collection::NO_UNIQUE_CHECKS | Collection::NO_LOCKTREE;
        rsOplogRefsDetails->insertObject(o, flags);
    }

    // assumes oplog is read locked on entry
    void replicateTransactionToOplog(BSONObj& op) {
        // set the applied bool to false, to let the oplog know that
        // this entry has not been applied to collections
        BSONElementManipulator(op["a"]).setBool(false);
        writeEntryToOplog(op, true);
    }

    // Copy a range of documents to the local oplog.refs collection
    static void copyOplogRefsRange(OplogReader &r, OID oid) {
        shared_ptr<DBClientCursor> c = r.getOplogRefsCursor(oid);
        LOCK_REASON(lockReason, "repl: copying oplog.refs range");
        Client::ReadContext ctx(rsOplogRefs, lockReason);
        while (c->more()) {
            BSONObj b = c->next();
            BSONElement eOID = b.getFieldDotted("_id.oid");
            if (oid != eOID.OID()) {
                break;
            }
            LOG(6) << "copyOplogRefsRange " << b << endl;
            writeEntryToOplogRefs(b);
        }
    }

    void replicateFullTransactionToOplog(BSONObj& o, OplogReader& r, bool* bigTxn) {
        *bigTxn = false;
        if (o.hasElement("ref")) {
            OID oid = o["ref"].OID();
            LOG(3) << "oplog ref " << oid << endl;
            copyOplogRefsRange(r, oid);
            *bigTxn = true;
        }

        LOCK_REASON(lockReason, "repl: copying entry to local oplog");
        Client::ReadContext ctx(rsoplog, lockReason);
        replicateTransactionToOplog(o);
    }

    // apply all operations in the array
    void applyOps(std::vector<BSONElement> ops) {
        const size_t numOps = ops.size();
        for(size_t i = 0; i < numOps; ++i) {
            BSONElement* curr = &ops[i];
            OpLogHelpers::applyOperationFromOplog(curr->Obj());
        }
    }

    // find all oplog entries for a given OID in the oplog.refs collection and apply them
    // TODO this should be a range query on oplog.refs where _id.oid == oid and applyOps to
    // each entry found.  The locking of the query interleaved with the locking in the applyOps
    // did not work, so it a sequence of point queries.  
    // TODO verify that the query plan is a indexed lookup.
    // TODO verify that the query plan does not fetch too many docs and then only process one of them.
    void applyRefOp(BSONObj entry) {
        OID oid = entry["ref"].OID();
        LOG(3) << "apply ref " << entry << " oid " << oid << endl;
        long long seq = 0; // note that 0 is smaller than any of the seq numbers
        while (1) {
            BSONObj entry;
            {
                LOCK_REASON(lockReason, "repl: finding oplog.refs entry to apply");
                Client::ReadContext ctx(rsOplogRefs, lockReason);
                const BSONObj query = BSON("_id" << BSON("$gt" << BSON("oid" << oid << "seq" << seq)));
                if (!Collection::findOne(rsOplogRefs, query, entry, true)) {
                    break;
                }
            }
            BSONElement e = entry.getFieldDotted("_id.seq");
            seq = e.Long();
            BSONElement eOID = entry.getFieldDotted("_id.oid");
            if (oid != eOID.OID()) {
                break;
            }
            LOG(3) << "apply " << entry << " seq=" << seq << endl;
            applyOps(entry["ops"].Array());
        }
    }
    
    // takes an entry that was written _logTransactionOps
    // and applies them to collections
    //
    // TODO: possibly improve performance of this. We create and destroy a
    // context for each operation. Find a way to amortize it out if necessary
    //
    void applyTransactionFromOplog(BSONObj entry) {
        bool transactionAlreadyApplied = entry["a"].Bool();
        if (!transactionAlreadyApplied) {
            Client::Transaction transaction(DB_SERIALIZABLE);
            if (entry.hasElement("ref")) {
                applyRefOp(entry);
            } else if (entry.hasElement("ops")) {
                applyOps(entry["ops"].Array());
            } else {
                verify(0);
            }
            // set the applied bool to true, to let the oplog know that
            // this entry has been applied to collections
            BSONElementManipulator(entry["a"]).setBool(true);
            {
                LOCK_REASON(lockReason, "repl: setting oplog entry's applied bit");
                Lock::DBRead lk1("local", lockReason);
                writeEntryToOplog(entry, false);
            }
            // If this code fails, it is impossible to recover from
            // because we don't know if the transaction successfully committed
            // so we might as well crash
            // There is currently no known way this code can throw an exception
            try {
                // we are operating as a secondary. We don't have to fsync
                transaction.commit(DB_TXN_NOSYNC);
            }
            catch (std::exception &e) {
                log() << "exception during commit of applyTransactionFromOplog, aborting system: " << e.what() << endl;
                printStackTrace();
                logflush();
                ::abort();
            }
        }
    }
    
    // apply all operations in the array
    void rollbackOps(std::vector<BSONElement> ops) {
        const size_t numOps = ops.size();
        for(size_t i = 0; i < numOps; ++i) {
            // note that we have to rollback the transaction backwards
            BSONElement* curr = &ops[numOps - i - 1];
            OpLogHelpers::rollbackOperationFromOplog(curr->Obj());
        }
    }

    void rollbackRefOp(BSONObj entry) {
        OID oid = entry["ref"].OID();
        LOG(3) << "rollback ref " << entry << " oid " << oid << endl;
        long long seq = LLONG_MAX;
        while (1) {
            BSONObj currEntry;
            {
                LOCK_REASON(lockReason, "repl: rolling back entry from oplog.refs");
                Client::ReadContext ctx(rsOplogRefs, lockReason);
                verify(rsOplogRefsDetails != NULL);
                shared_ptr<Cursor> c(
                    Cursor::make(
                        rsOplogRefsDetails,
                        rsOplogRefsDetails->getPKIndex(),
                        KeyPattern::toKeyFormat(BSON( "_id" << BSON("oid" << oid << "seq" << seq))), // right endpoint
                        KeyPattern::toKeyFormat(BSON( "_id" << BSON("oid" << oid << "seq" << 0))), // left endpoint
                        false,
                        -1 // direction
                        )
                    );
                if (c->ok()) {
                    currEntry = c->current().copy();
                }
                else {
                    break;
                }
            }
            BSONElement e = currEntry.getFieldDotted("_id.seq");
            seq = e.Long();
            BSONElement eOID = currEntry.getFieldDotted("_id.oid");
            if (oid != eOID.OID()) {
                break;
            }
            LOG(3) << "apply " << currEntry << " seq=" << seq << endl;
            rollbackOps(currEntry["ops"].Array());
            // decrement seq so next query gets the next value
            seq--;
        }
    }

    void rollbackTransactionFromOplog(BSONObj entry, bool purgeEntry) {
        bool transactionAlreadyApplied = entry["a"].Bool();
        Client::Transaction transaction(DB_SERIALIZABLE);
        if (transactionAlreadyApplied) {
            if (entry.hasElement("ref")) {
                rollbackRefOp(entry);
            } else if (entry.hasElement("ops")) {
                rollbackOps(entry["ops"].Array());
            } else {
                verify(0);
            }
        }
        {
            LOCK_REASON(lockReason, "repl: purging entry from oplog");
            Lock::DBRead lk1("local", lockReason);
            if (purgeEntry) {
                purgeEntryFromOplog(entry);
            }
            else {
                // set the applied bool to false, to let the oplog know that
                // this entry has not been applied to collections
                BSONElementManipulator(entry["a"]).setBool(false);
                writeEntryToOplog(entry, false);
            }
        }
        transaction.commit(DB_TXN_NOSYNC);
    }
    
    void purgeEntryFromOplog(BSONObj entry) {
        verify(rsOplogDetails);
        if (entry.hasElement("ref")) {
            OID oid = entry["ref"].OID();
            LOCK_REASON(lockReason, "repl: purging oplog.refs for oplog entry");
            Client::ReadContext ctx(rsOplogRefs, lockReason);
            Client::Transaction txn(DB_SERIALIZABLE);
            deleteIndexRange(
                rsOplogRefs,
                BSON("_id" << BSON("oid" << oid << "seq" << 0)),
                BSON("_id" << BSON("oid" << oid << "seq" << LLONG_MAX)),
                BSON("_id" << 1),
                true,
                false
                );
            txn.commit();
        }

        BSONObj pk = entry["_id"].wrap("");
        const uint64_t flags = Collection::NO_LOCKTREE;
        rsOplogDetails->deleteObject(pk, entry, flags);
    }

    uint64_t expireOplogMilliseconds() {
        const uint32_t days = cmdLine.expireOplogDays;
        const uint32_t hours = days * 24 + cmdLine.expireOplogHours;
        const uint64_t millisPerHour = 3600 * 1000;
        return hours * millisPerHour;
    }

    void hotOptimizeOplogTo(GTID gtid, const int timeout, uint64_t *loops_run) {
        LOCK_REASON(lockReason, "repl: optimizing oplog");
        Client::ReadContext ctx(rsoplog, lockReason);

        // do a hot optimize up until gtid;
        BSONObjBuilder q;
        addGTIDToBSON("", gtid, q);

        // TODO: rsOplogDetails should be stored as OplogCollection, not Collection
        OplogCollection *cl = rsOplogDetails->as<OplogCollection>();
        cl->optimizePK(minKey, q.done(), timeout, loops_run);
    }
}
