// SPDX-License-Identifier: GPL-2.0

//! Support for loading and patching the `Booter` firmware. `Booter` is a Heavy Secured firmware
//! running on [`Sec2`], that is used on Turing/Ampere and to load the GSP firmware into the GSP
//! falcon and unload it.

use core::marker::PhantomData;
use core::ops::Deref;

use kernel::device;
use kernel::firmware::Firmware;
use kernel::prelude::*;
use kernel::transmute::{FromBytes, FromBytesSized};

use crate::dma::DmaObject;
use crate::driver::Bar0;
use crate::falcon::sec2::Sec2;
use crate::falcon::FalconLoadParams;
use crate::falcon::{Falcon, FalconBromParams, FalconFirmware, FalconLoadTarget};
use crate::firmware::FirmwareSignature;
use crate::firmware::{
    BinHdr, FirmwareDmaObject, HsHeaderV2, HsLoadHeaderV2, HsLoadHeaderV2App, Signed, Unsigned,
};

/// Returns a copy of the instance of `S` by reinterpreting the bytes starting at `offset` in `fw`.
fn frombytes_at_offset<S: FromBytes + Sized>(fw: &Firmware, offset: usize) -> Result<S> {
    fw.data()
        .get(offset..offset + size_of::<S>())
        .and_then(S::from_bytes_copy)
        .ok_or(EINVAL)
}

/// Signature parameters, as defined in the firmware.
#[repr(C)]
struct HsSignatureParams {
    fuse_ver: u32,
    engine_id_mask: u32,
    ucode_id: u32,
}
unsafe impl FromBytesSized for HsSignatureParams {}

/// Signature for Booter firmware. Their size is encoded into the header and not known a compile
/// time, so we just wrap a byte slices on which we can implement [`FirmwareSignature`].
struct BooterSignature<'a>(&'a [u8]);

impl<'a> AsRef<[u8]> for BooterSignature<'a> {
    fn as_ref(&self) -> &[u8] {
        self.0
    }
}

impl<'a> FirmwareSignature<BooterFirmware> for BooterSignature<'a> {}

/// The `Booter` loader microcode, responsible for loading the GSP.
pub(crate) struct BooterFirmware {
    load_hdr: HsLoadHeaderV2,
    app0: HsLoadHeaderV2App,
    brom_params: FalconBromParams,
    ucode: FirmwareDmaObject<Self, Signed>,
}

impl FirmwareDmaObject<BooterFirmware, Unsigned> {
    fn new_booter(dev: &device::Device<device::Bound>, data: &[u8]) -> Result<Self> {
        DmaObject::from_data(dev, data).map(|ucode| Self(ucode, PhantomData))
    }
}

impl BooterFirmware {
    pub(crate) fn new(
        dev: &device::Device<device::Bound>,
        falcon: &Falcon<<Self as FalconFirmware>::Target>,
        bar: &Bar0,
        fw: &Firmware,
    ) -> Result<Self> {
        const BIN_MAGIC: u32 = 0x10de;

        let hdr = frombytes_at_offset::<BinHdr>(fw, 0)?;
        if hdr.bin_magic != BIN_MAGIC {
            dev_err!(dev, "invalid booter firmware header\n");
            return Err(EINVAL);
        }

        // Object containing the firmware to be signature-patched.
        let ucode = {
            let fw_start = hdr.data_offset as usize;
            let fw_size = hdr.data_size as usize;

            FirmwareDmaObject::<Self, _>::new_booter(
                dev,
                fw.data().get(fw_start..fw_start + fw_size).ok_or(EINVAL)?,
            )?
        };

        let hs_hdr = frombytes_at_offset::<HsHeaderV2>(fw, hdr.header_offset as usize)?;
        let load_hdr = frombytes_at_offset::<HsLoadHeaderV2>(fw, hs_hdr.header_offset as usize)?;
        let app0 = frombytes_at_offset::<HsLoadHeaderV2App>(
            fw,
            hs_hdr.header_offset as usize + size_of::<HsLoadHeaderV2>(),
        )?;
        let num_sig = frombytes_at_offset::<u32>(fw, hs_hdr.num_sig as usize)?;
        let patch_loc = frombytes_at_offset::<u32>(fw, hs_hdr.patch_loc as usize)?;
        let patch_sig = frombytes_at_offset::<u32>(fw, hs_hdr.patch_sig as usize)?;
        let sig_params =
            frombytes_at_offset::<HsSignatureParams>(fw, hs_hdr.meta_data_offset as usize)?;
        let brom_params = FalconBromParams {
            pkc_data_offset: patch_loc - load_hdr.os_data_offset,
            engine_id_mask: u16::try_from(sig_params.engine_id_mask).map_err(|_| EINVAL)?,
            ucode_id: u8::try_from(sig_params.ucode_id).map_err(|_| EINVAL)?,
        };

        // TODO: Extract signatures slice first? We can get `idx` later.

        let reg_fuse_version = {
            falcon.signature_reg_fuse_version(
                bar,
                brom_params.engine_id_mask,
                brom_params.ucode_id,
            )?
        };
        if sig_params.fuse_ver < reg_fuse_version {
            dev_err!(dev, "invalid fuse version for Booter firmware\n");
            return Err(EINVAL);
        }

        let ucode_signed = if num_sig != 0 {
            let idx = if reg_fuse_version != 0 {
                sig_params.fuse_ver - reg_fuse_version
            } else {
                num_sig - 1
            };

            let signature = {
                let sig_size = (hs_hdr.sig_prod_size / num_sig) as usize;
                let signatures_start = (hs_hdr.sig_prod_offset + patch_sig) as usize;

                fw.data()
                    // Get signatures range.
                    .get(signatures_start..signatures_start + hs_hdr.sig_prod_size as usize)
                    .ok_or(EINVAL)?
                    // Split into individual signatures.
                    .chunks_exact(sig_size)
                    .map(BooterSignature)
                    // Get signature `idx`.
                    .nth(idx as usize)
                    .ok_or(EINVAL)?
            };

            ucode.patch_signature(&signature, patch_loc as usize)?
        } else {
            ucode.no_patch_signature()
        };

        Ok(Self {
            load_hdr,
            app0,
            brom_params,
            ucode: ucode_signed,
        })
    }
}

impl FalconLoadParams for BooterFirmware {
    fn imem_load_params(&self) -> FalconLoadTarget {
        FalconLoadTarget {
            src_start: self.app0.offset,
            dst_start: 0,
            len: self.app0.len,
        }
    }

    fn dmem_load_params(&self) -> FalconLoadTarget {
        FalconLoadTarget {
            src_start: self.load_hdr.os_data_offset,
            dst_start: 0,
            len: self.load_hdr.os_data_size,
        }
    }

    fn brom_params(&self) -> FalconBromParams {
        self.brom_params.clone()
    }

    fn boot_addr(&self) -> u32 {
        self.app0.offset
    }
}

impl Deref for BooterFirmware {
    type Target = DmaObject;

    fn deref(&self) -> &Self::Target {
        &self.ucode.0
    }
}

impl FalconFirmware for BooterFirmware {
    type Target = Sec2;
}
