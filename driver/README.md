# TSNLAB XDMA 

This repository is a fork of the Xilinx DMA IP Reference drivers.
For Xilinx DMA IP Reference drivers, see Xilinx DMA IP Reference drivers below.

TSNLAB XDMA is in subdirectory XDMA/linux-kernel.
Go to the XDMA/linux-kernel directory and see the readme.txt file.

## SE images with 8-byte DMA padding

Use the DMA mode specified for your FPGA image. A PCI ID such as `10ee:7021`
or `10ee:7024` identifies a supported device but does not select its DMA mode.
For an SE Gen2 x1 image that requires 8-byte DMA transfers, load the driver
with the alignment option:

```sh
sudo insmod xdma.ko se_mode=1 tx_h2c_align=1
```

For other images, omit `tx_h2c_align` (the default is off). The driver aligns
the DMA source address and transfer length while keeping the original Ethernet
frame length. It removes the matching receive-side padding before passing the
frame to Linux. Set `tx_h2c_align` and `tx_bounce` when loading the module.

#

# Xilinx DMA IP Reference drivers

## Xilinx QDMA

The Xilinx PCI Express Multi Queue DMA (QDMA) IP provides high-performance direct memory access (DMA) via PCI Express. The PCIe QDMA can be implemented in UltraScale+ devices.

Both the linux kernel driver and the DPDK driver can be run on a PCI Express root port host PC to interact with the QDMA endpoint IP via PCI Express.

### Getting Started

* [QDMA Reference Drivers Comprehensive documentation](https://xilinx.github.io/dma_ip_drivers/)

## Xilinx-VSEC (XVSEC)

Xilinx-VSEC (XVSEC) are Xilinx supported VSECs. The XVSEC Driver helps creating and deploying designs that may include the Xilinx VSEC PCIe Features.

VSEC (Vendor Specific Extended Capability) is a feature of PCIe.

The VSEC itself is implemented in the PCIe extended capability register in the FPGA hardware (as either soft or hard IP). The drivers and SW are created to interface with and use this hardware implemented feature.

The XVSEC driver currently include the MCAP VSEC, but will be expanded to include the XVC VSEC and NULL VSEC.

### Getting Started

* [XVSEC Linux Kernel Reference Driver Comprehensive documentation](https://xilinx.github.io/dma_ip_drivers/)

### Support

Refer to Xilinx PCIe Forum for any queries/issues/support required w.r.t Xilinx's DMA IP Reference Drivers

Note: Issues are disabled in github for these drivers. All the queries shall be redirected through Xilinx PCIe Forum link given below.

* [Xilinx PCIe Forum](https://forums.xilinx.com/t5/PCIe-and-CPM/bd-p/PCIe)
