# Design notes for snapshot-based remote replication #

## Overview ##
Given a block level snapshot facility capable of holding multiple simultaneous snapshots efficiently, block device replication is easy. Just hack the snapshot code to generate a list of snapshot chunks that differ between two snapshots, then send that difference over the wire. On a downstream server, write the updated data to a snapshotted block device, and that is replication.

## Goals ##
  * Continuously replicate snapshots of an upstream server volume to one or more downstream servers.
  * Try to minimize transmitted data using differencing and compression.
  * Extra load on either upstream or downstream server during replication is insignificant.
  * Short code and easy to implement to maximize stability.
  * Runs (only) on Linux.
  * Minimal library dependencies.
  * Simple and flexible command line interface.

## Nongoals ##
  * Don't try to be too automagic in the initial prototype.

## Notes ##
  * The upstream server needs to remember the most recent snapshot replicated to each downstream server and avoid deleting it until a more recent snapshot has been replicated. Alternatively, the upstream server may extract a snapshot delta and store it offline.
  * We need to implement use counts in the snapshot server so that if the server ever needs to drop a snapshot automatically it will not choose to drop a snapshot involved in replication.
  * Snapshot server does not have a advance-to-next-leaf operation, we need this to traverse the snapshot metadata efficiently.
  * We can recover snapshot space on the upstream host by creating and storing a delta for any downstream host that can't receive the delta immediately.
  * If we ever end up in a situation where the upstream host does not hold or have deltas from a downstream host's latest snapshot then we would need to run an rsync-like process to update to a snapshot that upstream does hold, or alternatively replicate the full volume or physically transport a full volume copy.
  * We need to take account of the journal. We can either take an upstream snapshot at any random time and replay the journal on the downstream host, or we can flush the journal before taking any snapshot on the upstream host, this should be configurable. The upstream replay would interrupt write traffic for a few seconds on the upstream host, so the downstream replay might be preferable. This should be configurable.
  * Programming language: generating the list of changed blocks (just a list of logical addresses) needs to be part of the snapshot server, because it needs to be synchronized with updates to the snapshot metadata. This simple code will therefore be written in C. The remainder of the code on the upstream and downstream hosts may be reasonably written in a scripting language like Python. Or we might implement this as a simple set of shell commands coded in C(++) suitable to be driven from a scripting language (let's do it this way).
  * We can compress a blockwise delta optimally by taking a binary difference between source and destination chunks and optionally compressing the result. Compression can be global (slow!) or per some run of chunks so that the delta can still be transmitted as a stream.
  * An upstream snapshot that is the latest snapshot held by any downstream host is read only. The downstream origin volume is written only by the process that applies a volume delta. After a volume delta is applied to a downstream origin volume, the volume is snapshotted then the journal may be replayed if necessary, otherwise downstream snapshots are read only.
  * If we have more than one downstream host we may want to store the delta as a file upstream and transmit to several downstream hosts. Need some interface, perhaps a separate command line utility to apply a delta file instead of a stream, or maybe this is an option on the daemon: run as command with an input file, don't be a daemon. The command version is easiest to implement at first, we can use a standard utility to transmit the delta file and to run the apply command remotely. The disadvantage is, a delta file could be as big as the entire downstream volume. So we need the streaming version for production.
  * Should zero out a new volume and avoid transmitting zeros. Blockwise or global compression should take care of this nicely. Would be nice to avoid copy-before-write for zeroed chunks, and also to zero out freed chunks. "Further work". (VFS impact)
  * We can in theory compose deltas to save transmission time in certain cases. Composing means generating a new delta from two input deltas which is the union of the two input deltas with rewritten entries discarded. Since deltas are in logical address order, we could also merge on the fly without actually storing the composed files. Or we can mindlessly just apply several deltas in a row when necessary. It would be nice to have stats on what proportion of chunks are rewritten in successive deltas, hence what savings we can expect from composing.

## Implementation Notes ##
  * We follow Linux convention and express chunk addresses as 512 byte sectors, that is, chunk 1 has address 8, given 4096 byte chunks.
  * Structs and definitions that need to be shared between kernel and user space are declared in drivers/md/dm-ddraid.h.
  * Structs to be written to disk or over the network must be declared with attribute packed, see dm-ddraid.h
  * We need to make this endian neutral at some point. The defacto Linux rule: structs going over the wire or over the wire and to disk are big endian, structs going only to disk (e.g., filesystem) are little endian. Not to worry about this now.
  * Delta files will be stupidly large for real volumes. We will need to implement the streaming version before going into production, but these files will do for a ssh-based replication demo.

# Replication algorithm #
  1. Snapshot the upstream volume
  1. Copy the full snapshot volume to the downstream host (or physically transport)
  1. Snapshot the downstream volume
    * The downstream NFS server may now serve this initial snapshot to NFS clients
  1. Time to replicate. Set a new snapshot on the upstream volume.
  1. Obtain the changelist, a list of logical addresses of chunks that differ between the latest upstream snapshot and some upstream snapshot that is the same as the latest downstream snapshot.
  1. Transmit the changed chunks and logical addresses to the downstream host (compress this stream)
  1. Write the changed chunks to the downstream (origin) volume (overlap with 6)
  1. Set a new snapshot on the downstream volume
    * Optionally replay the journal on the latest downstream snapshot
    * The downstream NFS server may now serve the latest snapshot to NFS clients
    * Older downstream and upstream snapshots may now be discarded
    * Besides providing explicit snapshot volumes for NFS mounting, we also need to be able to "fast forward" an already mounted volume without disconnecting clients. We will likely need to fix some issues here, let's see.
  * Repeat at 4. for the next replication cycle

# Primitive Components #
  1. Capture changelist
    * Runs in upstream snapshot server
    * On request, writes the delta list between two read-only snapshots to a caller supplied FD (socket or file) as a stream of logical addresses of chunks that differ between the two snapshots
  1. Transmit delta
    * Transmit a volume delta to a remote host
    * Either be streaming or file based
      * Streaming
        * Delta will be generated to a socket and reformatted into a data stream by reading volume data from the higher numbered snapshot
      * File based
        * Similiar to streaming except the delta stream is written to a file
        * We gain the ability to prepend a header giving statistics such as number of changed blocks
        * Keeps track of most recent snapshot number of each downstream host.
  1. Apply delta
    * Read a delta stream and applies it to a volume (origin) on the downstream host
    * When done, notifies upstream that the higher numbered snapshot has been applied so that upstream may release the lower numbered snapshot.
    * We can avoid ever storing the delta stream itself on the downstream host by applying the changes as they arrive over the network, with the drawback that snapshotted data will be served off the spatially unordered snapshot store for a longer period.
  * We can combine the above operations manually or via scripting to carry out replication.

# Snapshot changelist capture algorithm #
Snapshot changelist capture is triggered by connecting to the snapshot server and issuing a message, which specifies two snapshot tags for the delta and passes a fd to which the snapshot server will write its output.
  * Message format: snapshot difference, snap0, snap1, fd (we use a unix domain socket so we can pass a fd for the result, the fd can be a socket or a file).
  * Ensure Snap1 is not open RW, set R/O
  * Take use count on snap0 and snap1
    * snap0 should already have a use count since it is held downstream, extra use count here does no harm and makes it more robust
  * Output list of logical addresses of chunks in snap1 that were written since snap0:
    * For each btree leaf
      * For each logical address in leaf
        * For each physical address in logical address list
          * If B1 is set and B0 is not set
            * Emit logical address
            * go to next logical address
  * Emit end of logical address list marker and close fd.
  * Drop the use count on snap0 and snap1

