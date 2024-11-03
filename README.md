# Fluxgate magnetometer datalogger

Data is logged to a microSD/SDHC/SDXC card, which must support SPI mode.
It should be formated as FAT12, FAT16, FAT32 or ExFAT.
If it has an MBR partion table, the first partion will be used.

Most sub 2 TB cards should work as is.

Data is logged to `FLUXGATE.CSV`, in a CSV format.

|First field|Second field format|Notes|
|-|-|-|
|empty string|empty string|Startup banner, includes third field with human readable comment|
|`Vdiv`|Int (mV)|Measured voltage on the Vdd/2 rail of the opamp filter, written on startup.|
|`Vamp`|Int (mV)|Measured amplifier output voltage, written on startup|
|`Vdiv`|Int (mV)|Measure diffence between Vdd/2 and output, written on startup|
|`OSR`|Int|Oversampling ratio used for measurements|
|`Tlog`|Int (ms)|Time between measurements|
|Int|Int|Field measurements, first field is a counter that increments with each one.|

$$ \text{Reading} = \text{Peak-Peak voltage (mV)} \times 10 \times \text{OSR} $$

If a file called `FLUXGATE.CFG` exists, it is read to configure the sensor, in a binary format, each field is little endian:

|Length (bits)|Value|Range|
|-|-|
|32|Sample delay, millisconds|0-30 seconds
|32|Oversampling ratio|1-13100|

# Hardware

It should be able to drive most fluxgates, but I got the best result with a [high permiability, square-loop core](https://www.digikey.com/en/products/detail/toshiba-semiconductor-and-storage/MS21X14X4-5W/4701157)
using the classic ring core configuration:
The drive coil was wound around the entire core, and the sense coil was wound on a 3d printed bobin placed over the core:

![](fluxgate.jpg) 
 
With the high gain of the amplifier, the earth's magnetic field (50 uT at my place) is enough to saturate the sensor. 
Alternativly, the compensation driver can be enabled to cancel out the field, but the driver can only push current in one direction, so the sensor might have to be flipped for this to work.

