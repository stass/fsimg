# SPDX-License-Identifier: BSD-2-Clause
# Copyright (c) 2026, fsimg contributors
# frozen_string_literal: true

require "bindata"

module FsimgSpec
  module Fat
    # The on-disk records of the Microsoft FAT specification (section 3,
    # "Boot Sector and BPB"; section 5, "File System Information (FSInfo)
    # Structure"; section 6, "Directory Structure"), written from that
    # document and named after its fields in snake case.
    # Each record starts at its own position: the common BPB at byte 0 of
    # the volume, one of the two extensions at byte 36, the signature at 510,
    # FSInfo at the start of its sector. Layout code never rejects a value.
    module Layout
      # BS_jmpBoot through BPB_TotSec32, bytes 0-35 of the boot sector
      # (section 3.1).
      class CommonBpb < BinData::Record
        endian :little
        string :bs_jmp_boot, length: 3
        string :bs_oem_name, length: 8
        uint16 :bpb_byts_per_sec
        uint8 :bpb_sec_per_clus
        uint16 :bpb_rsvd_sec_cnt
        uint8 :bpb_num_fats
        uint16 :bpb_root_ent_cnt
        uint16 :bpb_tot_sec16
        uint8 :bpb_media
        uint16 :bpb_fat_sz16
        uint16 :bpb_sec_per_trk
        uint16 :bpb_num_heads
        uint32 :bpb_hidd_sec
        uint32 :bpb_tot_sec32
      end

      # The FAT12 and FAT16 extension, bytes 36-61 (section 3.2). The boot
      # code that follows is not decoded.
      class Fat16Extension < BinData::Record
        endian :little
        uint8 :bs_drv_num
        uint8 :bs_reserved1
        uint8 :bs_boot_sig
        uint32 :bs_vol_id
        string :bs_vol_lab, length: 11
        string :bs_fil_sys_type, length: 8
      end

      # The FAT32 extension, bytes 36-89 (section 3.3). It never overlays a
      # FAT12 or FAT16 boot sector; the model chooses one extension.
      class Fat32Extension < BinData::Record
        endian :little
        uint32 :bpb_fat_sz32
        uint16 :bpb_ext_flags
        uint16 :bpb_fs_ver
        uint32 :bpb_root_clus
        uint16 :bpb_fs_info
        uint16 :bpb_bk_boot_sec
        string :bpb_reserved, length: 12
        uint8 :bs_drv_num
        uint8 :bs_reserved1
        uint8 :bs_boot_sig
        uint32 :bs_vol_id
        string :bs_vol_lab, length: 11
        string :bs_fil_sys_type, length: 8
      end

      # Signature_word at offset 510 of the boot sector: 0x55 at 510 and
      # 0xAA at 511, so 0xAA55 read little-endian (sections 3.2 and 3.3).
      class Signature < BinData::Record
        endian :little
        uint16 :signature_word
      end

      # The FSInfo sector of a FAT32 volume (section 5).
      class FsInfo < BinData::Record
        endian :little
        uint32 :fsi_lead_sig
        string :fsi_reserved1, length: 480
        uint32 :fsi_struc_sig
        uint32 :fsi_free_count
        uint32 :fsi_nxt_free
        string :fsi_reserved2, length: 12
        uint32 :fsi_trail_sig
      end

      # One 32-byte directory entry (section 6).
      class DirectoryEntry < BinData::Record
        endian :little
        string :dir_name, length: 11
        uint8 :dir_attr
        uint8 :dir_nt_res
        uint8 :dir_crt_time_tenth
        uint16 :dir_crt_time
        uint16 :dir_crt_date
        uint16 :dir_lst_acc_date
        uint16 :dir_fst_clus_hi
        uint16 :dir_wrt_time
        uint16 :dir_wrt_date
        uint16 :dir_fst_clus_lo
        uint32 :dir_file_size
      end

      # Where each record starts: the boot sector records relative to the
      # start of the volume, FSInfo to the start of its sector and a
      # directory entry to the start of the entry.
      POSITIONS = {
        CommonBpb => 0, Fat16Extension => 36, Fat32Extension => 36,
        Signature => 510, FsInfo => 0, DirectoryEntry => 0
      }.freeze

      # One frozen Data class per record, generated from the record's field
      # table, with the record's fields as members. The model holds these
      # plain values, never BinData objects, so a rule cannot change what
      # another rule reads, and values compare, hash and pattern-match as
      # Integers and Strings.
      VALUES = POSITIONS.keys.to_h { [_1, Data.define(*_1.fields.field_names)] }.freeze

      # The plain values of record decoded from bytes, which must hold the
      # whole record.
      def self.decode(record, bytes)
        fields = record.read(bytes).snapshot.transform_values { _1.is_a?(String) ? _1.b.freeze : _1 }
        VALUES.fetch(record).new(**fields)
      end

      # The offset of every field from the record's position, by record:
      # { CommonBpb => { bpb_byts_per_sec: 11, ... }, ... }.
      def self.field_offsets
        POSITIONS.to_h do |record, position|
          sample = record.new
          [record, record.fields.field_names.to_h { [_1, position + sample.public_send(_1).rel_offset] }]
        end
      end
    end
  end
end
