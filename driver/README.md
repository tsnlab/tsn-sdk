# TSNLAB XDMA 

This repository is a fork of the Xilinx DMA IP Reference drivers.
For Xilinx DMA IP Reference drivers, see Xilinx DMA IP Reference drivers below.

TSNLAB XDMA is in subdirectory XDMA/linux-kernel.
Go to the XDMA/linux-kernel directory and see the readme.txt file.

## SE driver installation

Follow the [Raspberry Pi 5 installation guide](XDMA/linux-kernel/README_TSN_HAT.md)
for kernel headers, PCIe setup and module loading. In SE mode the driver uses
8-byte DMA alignment by default. This covers the SE-HP HAT C1 (Gen2 x1) image,
which requires aligned transfers. You can confirm the C1 image in the HAT's
release information. A PCI device ID or negotiated link width alone does not
identify the FPGA image.

The driver preserves the Ethernet frame length and removes receive padding
when the DMA length and frame metadata agree. SE network interfaces reflect
the physical link while administratively up. Use the current `tsnctl.py link`
to read the PHY when the driver manages it.

### Troubleshooting

For a diagnostic comparison, `tx_h2c_align=0` restores the unaligned path.
Keep the default for normal C1 operation. The DMA options take effect at
module load time. The alignment path uses coherent slots automatically.

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
