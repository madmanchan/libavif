// Copyright 2022 Google LLC. All rights reserved.
// SPDX-License-Identifier: BSD-2-Clause

#include "avif/internal.h"

#include <stdint.h>
#include <string.h>

avifResult avifGetExifTiffHeaderOffset(const uint8_t * exif, size_t exifSize, size_t * offset)
{
    const uint8_t tiffHeaderBE[4] = { 'M', 'M', 0, 42 };
    const uint8_t tiffHeaderLE[4] = { 'I', 'I', 42, 0 };
    exifSize = AVIF_MIN(exifSize, UINT32_MAX);
    for (*offset = 0; *offset + 4 < exifSize; ++*offset) {
        if (!memcmp(&exif[*offset], tiffHeaderBE, 4) || !memcmp(&exif[*offset], tiffHeaderLE, 4)) {
            return AVIF_RESULT_OK;
        }
    }
    // Couldn't find the TIFF header
    return AVIF_RESULT_INVALID_EXIF_PAYLOAD;
}

// Returns the offset to the Exif 8-bit orientation value and AVIF_RESULT_OK, or an error.
// If the offset is set to exifSize, there was no parsing error but no orientation tag was found.
static avifResult avifGetExifOrientationOffset(const uint8_t * exif, size_t exifSize, size_t * offset)
{
    const avifResult result = avifGetExifTiffHeaderOffset(exif, exifSize, offset);
    if (result != AVIF_RESULT_OK) {
        // Couldn't find the TIFF header
        return result;
    }

    avifROData raw = { exif + *offset, exifSize - *offset };
    const avifBool littleEndian = (raw.data[0] == 'I');
    avifROStream stream;
    avifROStreamStart(&stream, &raw, NULL, NULL);

    // TIFF Header
    uint32_t offsetTo0thIfd;
    if (!avifROStreamSkip(&stream, 4) || // Skip tiffHeaderBE or tiffHeaderLE.
        !avifROStreamReadU32Endianness(&stream, &offsetTo0thIfd, littleEndian)) {
        return AVIF_RESULT_INVALID_EXIF_PAYLOAD;
    }

    avifROStreamSetOffset(&stream, offsetTo0thIfd);
    uint16_t fieldCount;
    if (!avifROStreamReadU16Endianness(&stream, &fieldCount, littleEndian)) {
        return AVIF_RESULT_INVALID_EXIF_PAYLOAD;
    }
    for (uint16_t field = 0; field < fieldCount; ++field) { // for each field interoperability array
        uint16_t tag;
        uint16_t type;
        uint32_t count;
        uint16_t firstHalfOfValueOffset;
        if (!avifROStreamReadU16Endianness(&stream, &tag, littleEndian) || !avifROStreamReadU16Endianness(&stream, &type, littleEndian) ||
            !avifROStreamReadU32Endianness(&stream, &count, littleEndian) ||
            !avifROStreamReadU16Endianness(&stream, &firstHalfOfValueOffset, littleEndian) || !avifROStreamSkip(&stream, 2)) {
            return AVIF_RESULT_INVALID_EXIF_PAYLOAD;
        }
        // Orientation attribute according to JEITA CP-3451C section 4.6.4 (TIFF Rev. 6.0 Attribute Information):
        const uint16_t shortType = 0x03;
        if (tag == 0x0112 && type == shortType && count == 0x01) {
            // Only consider non-reserved orientation values, so that it is known that
            // the most meaningful byte of firstHalfOfValueOffset is 0.
            if (firstHalfOfValueOffset >= 1 && firstHalfOfValueOffset <= 8) {
                // Offset to the least meaningful byte of firstHalfOfValueOffset.
                *offset += avifROStreamOffset(&stream) - (littleEndian ? 4 : 3);
                return AVIF_RESULT_OK;
            }
        }
    }
    // Orientation is in the 0th IFD, so no need to parse the following ones.

    *offset = exifSize; // Signal missing orientation tag in valid Exif payload.
    return AVIF_RESULT_OK;
}

avifResult avifImageExtractExifOrientationToIrotImir(avifImage * image)
{
    const avifTransformFlags otherFlags = image->transformFlags & ~(AVIF_TRANSFORM_IROT | AVIF_TRANSFORM_IMIR);
    size_t offset;
    const avifResult result = avifGetExifOrientationOffset(image->exif.data, image->exif.size, &offset);
    if (result != AVIF_RESULT_OK) {
        return result;
    }
    if (offset < image->exif.size) {
        const uint8_t orientation = image->exif.data[offset];
        // Mapping from Exif orientation as defined in JEITA CP-3451C section 4.6.4.A Orientation
        // to irot and imir boxes as defined in HEIF ISO/IEC 28002-12:2021 sections 6.5.10 and 6.5.12.
        switch (orientation) {
            case 1: // The 0th row is at the visual top of the image, and the 0th column is the visual left-hand side.
                image->transformFlags = otherFlags;
                image->irot.angle = 0; // ignored
                image->imir.mode = 0;  // ignored
                return AVIF_RESULT_OK;
            case 2: // The 0th row is at the visual top of the image, and the 0th column is the visual right-hand side.
                image->transformFlags = otherFlags | AVIF_TRANSFORM_IMIR;
                image->irot.angle = 0; // ignored
                image->imir.mode = 1;
                return AVIF_RESULT_OK;
            case 3: // The 0th row is at the visual bottom of the image, and the 0th column is the visual right-hand side.
                image->transformFlags = otherFlags | AVIF_TRANSFORM_IROT;
                image->irot.angle = 2;
                image->imir.mode = 0; // ignored
                return AVIF_RESULT_OK;
            case 4: // The 0th row is at the visual bottom of the image, and the 0th column is the visual left-hand side.
                image->transformFlags = otherFlags | AVIF_TRANSFORM_IMIR;
                image->irot.angle = 0; // ignored
                image->imir.mode = 0;
                return AVIF_RESULT_OK;
            case 5: // The 0th row is the visual left-hand side of the image, and the 0th column is the visual top.
                image->transformFlags = otherFlags | AVIF_TRANSFORM_IROT | AVIF_TRANSFORM_IMIR;
                image->irot.angle = 1; // applied before imir according to MIAF spec ISO/IEC 28002-12:2021 - section 7.3.6.7
                image->imir.mode = 0;
                return AVIF_RESULT_OK;
            case 6: // The 0th row is the visual right-hand side of the image, and the 0th column is the visual top.
                image->transformFlags = otherFlags | AVIF_TRANSFORM_IROT;
                image->irot.angle = 3;
                image->imir.mode = 0; // ignored
                return AVIF_RESULT_OK;
            case 7: // The 0th row is the visual right-hand side of the image, and the 0th column is the visual bottom.
                image->transformFlags = otherFlags | AVIF_TRANSFORM_IROT | AVIF_TRANSFORM_IMIR;
                image->irot.angle = 3; // applied before imir according to MIAF spec ISO/IEC 28002-12:2021 - section 7.3.6.7
                image->imir.mode = 0;
                return AVIF_RESULT_OK;
            case 8: // The 0th row is the visual left-hand side of the image, and the 0th column is the visual bottom.
                image->transformFlags = otherFlags | AVIF_TRANSFORM_IROT;
                image->irot.angle = 1;
                image->imir.mode = 0; // ignored
                return AVIF_RESULT_OK;
            default: // reserved
                break;
        }
    }

    // The orientation tag is not mandatory (only recommended) according to JEITA CP-3451C section 4.6.8.A.
    // The default value is 1 if the orientation tag is missing, meaning:
    //   The 0th row is at the visual top of the image, and the 0th column is the visual left-hand side.
    image->transformFlags = otherFlags;
    image->irot.angle = 0; // ignored
    image->imir.mode = 0;  // ignored
    return AVIF_RESULT_OK;
}

uint8_t avifImageGetExifOrientationFromIrotImir(const avifImage * image)
{
    if ((image->transformFlags & AVIF_TRANSFORM_IROT) && (image->irot.angle == 1)) {
        if (image->transformFlags & AVIF_TRANSFORM_IMIR) {
            if (image->imir.mode) {
                return 7; // 90 degrees anti-clockwise then swap left and right.
            }
            return 5; // 90 degrees anti-clockwise then swap top and bottom.
        }
        return 6; // 90 degrees anti-clockwise.
    }
    if ((image->transformFlags & AVIF_TRANSFORM_IROT) && (image->irot.angle == 2)) {
        if (image->transformFlags & AVIF_TRANSFORM_IMIR) {
            if (image->imir.mode) {
                return 4; // 180 degrees anti-clockwise then swap left and right.
            }
            return 2; // 180 degrees anti-clockwise then swap top and bottom.
        }
        return 3; // 180 degrees anti-clockwise.
    }
    if ((image->transformFlags & AVIF_TRANSFORM_IROT) && (image->irot.angle == 3)) {
        if (image->transformFlags & AVIF_TRANSFORM_IMIR) {
            if (image->imir.mode) {
                return 5; // 270 degrees anti-clockwise then swap left and right.
            }
            return 7; // 270 degrees anti-clockwise then swap top and bottom.
        }
        return 8; // 270 degrees anti-clockwise.
    }
    if (image->transformFlags & AVIF_TRANSFORM_IMIR) {
        if (image->imir.mode) {
            return 2; // Swap left and right.
        }
        return 4; // Swap top and bottom.
    }
    return 1; // Default orientation ("top-left", no-op).
}

avifResult avifSetExifOrientation(avifRWData * exif, uint8_t orientation)
{
    size_t offset;
    const avifResult result = avifGetExifOrientationOffset(exif->data, exif->size, &offset);
    if (result != AVIF_RESULT_OK) {
        return result;
    }
    if (offset < exif->size) {
        exif->data[offset] = orientation;
        return AVIF_RESULT_OK;
    }
    // No Exif orientation was found.
    if (orientation == 1) {
        // The default orientation is 1, so if the given orientation is 1 too, do nothing.
        return AVIF_RESULT_OK;
    }
    // Adding an orientation tag to an Exif payload is involved.
    return AVIF_RESULT_NOT_IMPLEMENTED;
}

void avifImageSetMetadataExif(avifImage * image, const uint8_t * exif, size_t exifSize)
{
    avifRWDataSet(&image->exif, exif, exifSize);
    // Ignore any Exif parsing failure.
    (void)avifImageExtractExifOrientationToIrotImir(image);
}
