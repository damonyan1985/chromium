// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "printing/backend/cups_helper.h"

#include "printing/backend/print_backend.h"
#include "printing/printing_utils.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace printing {

namespace {

// Returns true if the papers have the same name, vendor ID, and size.
bool PapersEqual(const PrinterSemanticCapsAndDefaults::Paper& lhs,
                 const PrinterSemanticCapsAndDefaults::Paper& rhs) {
  return lhs.display_name == rhs.display_name &&
         lhs.vendor_id == rhs.vendor_id && lhs.size_um == rhs.size_um;
}

}  // namespace

TEST(PrintBackendCupsHelperTest, TestPpdParsingNoColorDuplexShortEdge) {
  constexpr char kTestPpdData[] =
      R"(*PPD-Adobe: "4.3"
*OpenGroup: General/General
*OpenUI *ColorModel/Color Model: PickOne
*DefaultColorModel: Gray
*ColorModel Gray/Grayscale: "
  <</cupsColorSpace 0/cupsColorOrder 0>>setpagedevice"
*ColorModel Black/Inverted Grayscale: "
  <</cupsColorSpace 3/cupsColorOrder 0>>setpagedevice"
*CloseUI: *ColorModel
*OpenUI *Duplex/2-Sided Printing: PickOne
*DefaultDuplex: DuplexTumble
*Duplex None/Off: "
  <</Duplex false>>setpagedevice"
*Duplex DuplexNoTumble/LongEdge: "
  </Duplex true/Tumble false>>setpagedevice"
*Duplex DuplexTumble/ShortEdge: "
  <</Duplex true/Tumble true>>setpagedevice"
*CloseUI: *Duplex
*CloseGroup: General)";

  PrinterSemanticCapsAndDefaults caps;
  EXPECT_TRUE(ParsePpdCapabilities("test", "", kTestPpdData, &caps));
  EXPECT_TRUE(caps.collate_capable);
  EXPECT_TRUE(caps.collate_default);
  EXPECT_TRUE(caps.copies_capable);
  EXPECT_THAT(caps.duplex_modes,
              testing::UnorderedElementsAre(SIMPLEX, LONG_EDGE, SHORT_EDGE));
  EXPECT_EQ(SHORT_EDGE, caps.duplex_default);
  EXPECT_FALSE(caps.color_changeable);
  EXPECT_FALSE(caps.color_default);
}

// Test duplex detection code, which regressed in http://crbug.com/103999.
TEST(PrintBackendCupsHelperTest, TestPpdParsingNoColorDuplexSimples) {
  constexpr char kTestPpdData[] =
      R"(*PPD-Adobe: "4.3"
*OpenGroup: General/General
*OpenUI *Duplex/Double-Sided Printing: PickOne
*DefaultDuplex: None
*Duplex None/Off: "
  <</Duplex false>>setpagedevice"
*Duplex DuplexNoTumble/Long Edge (Standard): "
  <</Duplex true/Tumble false>>setpagedevice"
*Duplex DuplexTumble/Short Edge (Flip): "
  <</Duplex true/Tumble true>>setpagedevice"
*CloseUI: *Duplex
*CloseGroup: General)";

  PrinterSemanticCapsAndDefaults caps;
  EXPECT_TRUE(ParsePpdCapabilities("test", "", kTestPpdData, &caps));
  EXPECT_TRUE(caps.collate_capable);
  EXPECT_TRUE(caps.collate_default);
  EXPECT_TRUE(caps.copies_capable);
  EXPECT_THAT(caps.duplex_modes,
              testing::UnorderedElementsAre(SIMPLEX, LONG_EDGE, SHORT_EDGE));
  EXPECT_EQ(SIMPLEX, caps.duplex_default);
  EXPECT_FALSE(caps.color_changeable);
  EXPECT_FALSE(caps.color_default);
}

TEST(PrintBackendCupsHelperTest, TestPpdParsingNoColorNoDuplex) {
  constexpr char kTestPpdData[] =
      R"(*PPD-Adobe: "4.3"
*OpenGroup: General/General
*OpenUI *ColorModel/Color Model: PickOne
*DefaultColorModel: Gray
*ColorModel Gray/Grayscale: "
  <</cupsColorSpace 0/cupsColorOrder 0>>setpagedevice"
*ColorModel Black/Inverted Grayscale: "
  <</cupsColorSpace 3/cupsColorOrder 0>>setpagedevice"
*CloseUI: *ColorModel
*CloseGroup: General)";

  PrinterSemanticCapsAndDefaults caps;
  EXPECT_TRUE(ParsePpdCapabilities("test", "", kTestPpdData, &caps));
  EXPECT_TRUE(caps.collate_capable);
  EXPECT_TRUE(caps.collate_default);
  EXPECT_TRUE(caps.copies_capable);
  EXPECT_THAT(caps.duplex_modes, testing::UnorderedElementsAre());
  EXPECT_EQ(UNKNOWN_DUPLEX_MODE, caps.duplex_default);
  EXPECT_FALSE(caps.color_changeable);
  EXPECT_FALSE(caps.color_default);
}

TEST(PrintBackendCupsHelperTest, TestPpdParsingColorTrueDuplexShortEdge) {
  constexpr char kTestPpdData[] =
      R"(*PPD-Adobe: "4.3"
*ColorDevice: True
*DefaultColorSpace: CMYK
*OpenGroup: General/General
*OpenUI *ColorModel/Color Model: PickOne
*DefaultColorModel: CMYK
*ColorModel CMYK/Color: "(cmyk) RCsetdevicecolor"
*ColorModel Gray/Black and White: "(gray) RCsetdevicecolor"
*CloseUI: *ColorModel
*OpenUI *Duplex/2-Sided Printing: PickOne
*DefaultDuplex: DuplexTumble
*Duplex None/Off: "
  <</Duplex false>>setpagedevice"
*Duplex DuplexNoTumble/LongEdge: "
  <</Duplex true/Tumble false>>setpagedevice"
*Duplex DuplexTumble/ShortEdge: "
  <</Duplex true/Tumble true>>setpagedevice"
*CloseUI: *Duplex
*CloseGroup: General)";

  PrinterSemanticCapsAndDefaults caps;
  EXPECT_TRUE(ParsePpdCapabilities("test", "", kTestPpdData, &caps));
  EXPECT_TRUE(caps.collate_capable);
  EXPECT_TRUE(caps.collate_default);
  EXPECT_TRUE(caps.copies_capable);
  EXPECT_THAT(caps.duplex_modes,
              testing::UnorderedElementsAre(SIMPLEX, LONG_EDGE, SHORT_EDGE));
  EXPECT_EQ(SHORT_EDGE, caps.duplex_default);
  EXPECT_TRUE(caps.color_changeable);
  EXPECT_TRUE(caps.color_default);
}

TEST(PrintBackendCupsHelperTest, TestPpdParsingColorFalseDuplexLongEdge) {
  constexpr char kTestPpdData[] =
      R"(*PPD-Adobe: "4.3"
*ColorDevice: True
*DefaultColorSpace: CMYK
*OpenGroup: General/General
*OpenUI *ColorModel/Color Model: PickOne
*DefaultColorModel: Grayscale
*ColorModel Color/Color: "
  %% FoomaticRIPOptionSetting: ColorModel=Color"
*FoomaticRIPOptionSetting ColorModel=Color: "
  JCLDatamode=Color GSCmdLine=Color"
*ColorModel Grayscale/Grayscale: "
  %% FoomaticRIPOptionSetting: ColorModel=Grayscale"
*FoomaticRIPOptionSetting ColorModel=Grayscale: "
  JCLDatamode=Grayscale GSCmdLine=Grayscale"
*CloseUI: *ColorModel
*OpenUI *Duplex/2-Sided Printing: PickOne
*DefaultDuplex: DuplexNoTumble
*Duplex None/Off: "
  <</Duplex false>>setpagedevice"
*Duplex DuplexNoTumble/LongEdge: "
  <</Duplex true/Tumble false>>setpagedevice"
*Duplex DuplexTumble/ShortEdge: "
  <</Duplex true/Tumble true>>setpagedevice"
*CloseUI: *Duplex
*CloseGroup: General)";

  PrinterSemanticCapsAndDefaults caps;
  EXPECT_TRUE(ParsePpdCapabilities("test", "", kTestPpdData, &caps));
  EXPECT_TRUE(caps.collate_capable);
  EXPECT_TRUE(caps.collate_default);
  EXPECT_TRUE(caps.copies_capable);
  EXPECT_THAT(caps.duplex_modes,
              testing::UnorderedElementsAre(SIMPLEX, LONG_EDGE, SHORT_EDGE));
  EXPECT_EQ(LONG_EDGE, caps.duplex_default);
  EXPECT_TRUE(caps.color_changeable);
  EXPECT_FALSE(caps.color_default);
}

TEST(PrintBackendCupsHelperTest, TestPpdParsingPageSize) {
  constexpr char kTestPpdData[] =
      R"(*PPD-Adobe: "4.3"
*OpenUI *PageSize: PickOne
*DefaultPageSize: Legal
*PageSize Letter/US Letter: "
  <</DeferredMediaSelection true /PageSize [612 792]
  /ImagingBBox null /MediaClass null >> setpagedevice"
*End
*PageSize Legal/US Legal: "
  <</DeferredMediaSelection true /PageSize [612 1008]
  /ImagingBBox null /MediaClass null >> setpagedevice"
*End
*DefaultPaperDimension: Legal
*PaperDimension Letter/US Letter: "612   792"
*PaperDimension Legal/US Legal: "612  1008"
*CloseUI: *PageSize)";

  PrinterSemanticCapsAndDefaults caps;
  EXPECT_TRUE(ParsePpdCapabilities("test", "", kTestPpdData, &caps));
  ASSERT_EQ(2UL, caps.papers.size());
  EXPECT_EQ("Letter", caps.papers[0].vendor_id);
  EXPECT_EQ("US Letter", caps.papers[0].display_name);
  EXPECT_EQ(215900, caps.papers[0].size_um.width());
  EXPECT_EQ(279400, caps.papers[0].size_um.height());
  EXPECT_EQ("Legal", caps.papers[1].vendor_id);
  EXPECT_EQ("US Legal", caps.papers[1].display_name);
  EXPECT_EQ(215900, caps.papers[1].size_um.width());
  EXPECT_EQ(355600, caps.papers[1].size_um.height());
  EXPECT_TRUE(PapersEqual(caps.papers[1], caps.default_paper));
}

TEST(PrintBackendCupsHelperTest, TestPpdParsingPageSizeNoDefaultSpecified) {
  constexpr char kTestPpdData[] =
      R"(*PPD-Adobe: "4.3"
*OpenUI *PageSize: PickOne
*PageSize A3/ISO A3: "
  << /DeferredMediaSelection true /PageSize [842 1191]
  /ImagingBBox null >> setpagedevice"
*End
*PageSize A4/ISO A4: "
  << /DeferredMediaSelection true /PageSize [595 842]
  /ImagingBBox null >> setpagedevice"
*End
*PageSize Legal/US Legal: "
  << /DeferredMediaSelection true /PageSize [612 1008]
  /ImagingBBox null >> setpagedevice"
*End
*PageSize Letter/US Letter: "
  << /DeferredMediaSelection true /PageSize [612 792]
  /ImagingBBox null >> setpagedevice"
*End
*PaperDimension A3/ISO A3: "842 1191"
*PaperDimension A4/ISO A4: "595 842"
*PaperDimension Legal/US Legal: "612 1008"
*PaperDimension Letter/US Letter: "612 792"
*CloseUI: *PageSize)";

  {
    PrinterSemanticCapsAndDefaults caps;
    EXPECT_TRUE(ParsePpdCapabilities("test", "en-US", kTestPpdData, &caps));
    ASSERT_EQ(4UL, caps.papers.size());
    EXPECT_EQ("Letter", caps.papers[3].vendor_id);
    EXPECT_EQ("US Letter", caps.papers[3].display_name);
    EXPECT_EQ(215900, caps.papers[3].size_um.width());
    EXPECT_EQ(279400, caps.papers[3].size_um.height());
    EXPECT_TRUE(PapersEqual(caps.papers[3], caps.default_paper));
  }
  {
    PrinterSemanticCapsAndDefaults caps;
    EXPECT_TRUE(ParsePpdCapabilities("test", "en-UK", kTestPpdData, &caps));
    ASSERT_EQ(4UL, caps.papers.size());
    EXPECT_EQ("A4", caps.papers[1].vendor_id);
    EXPECT_EQ("ISO A4", caps.papers[1].display_name);
    EXPECT_EQ(209903, caps.papers[1].size_um.width());
    EXPECT_EQ(297039, caps.papers[1].size_um.height());
    EXPECT_TRUE(PapersEqual(caps.papers[1], caps.default_paper));
  }
}

TEST(PrintBackendCupsHelperTest, TestPpdParsingBrotherPrinters) {
  {
    constexpr char kTestPpdData[] =
        R"(*PPD-Adobe: "4.3"
*ColorDevice: True
*OpenUI *BRPrintQuality/Color/Mono: PickOne
*DefaultBRPrintQuality: Auto
*BRPrintQuality Auto/Auto: ""
*BRPrintQuality Color/Color: ""
*BRPrintQuality Black/Mono: ""
*CloseUI: *BRPrintQuality)";

    PrinterSemanticCapsAndDefaults caps;
    EXPECT_TRUE(ParsePpdCapabilities("test", "", kTestPpdData, &caps));
    EXPECT_TRUE(caps.color_changeable);
    EXPECT_TRUE(caps.color_default);
    EXPECT_EQ(BROTHER_BRSCRIPT3_COLOR, caps.color_model);
    EXPECT_EQ(BROTHER_BRSCRIPT3_BLACK, caps.bw_model);
  }
  {
    constexpr char kTestPpdData[] =
        R"(*PPD-Adobe: "4.3"
*ColorDevice: True
*OpenUI *BRMonoColor/Color / Mono: PickOne
*DefaultBRMonoColor: Auto
*BRMonoColor Auto/Auto: ""
*BRMonoColor FullColor/Color: ""
*BRMonoColor Mono/Mono: ""
*CloseUI: *BRMonoColor)";

    PrinterSemanticCapsAndDefaults caps;
    EXPECT_TRUE(ParsePpdCapabilities("test", "", kTestPpdData, &caps));
    EXPECT_TRUE(caps.color_changeable);
    EXPECT_TRUE(caps.color_default);
    EXPECT_EQ(BROTHER_CUPS_COLOR, caps.color_model);
    EXPECT_EQ(BROTHER_CUPS_MONO, caps.bw_model);
  }
  {
    constexpr char kTestPpdData[] =
        R"(*PPD-Adobe: "4.3"
*ColorDevice: True
*OpenUI *BRDuplex/Two-Sided Printing: PickOne
*DefaultBRDuplex: DuplexTumble
*BRDuplex DuplexTumble/Short-Edge Binding: ""
*BRDuplex DuplexNoTumble/Long-Edge Binding: ""
*BRDuplex None/Off: ""
*CloseUI: *BRDuplex)";

    PrinterSemanticCapsAndDefaults caps;
    EXPECT_TRUE(ParsePpdCapabilities("test", "", kTestPpdData, &caps));
    EXPECT_THAT(caps.duplex_modes,
                testing::UnorderedElementsAre(SIMPLEX, LONG_EDGE, SHORT_EDGE));
    EXPECT_EQ(SHORT_EDGE, caps.duplex_default);
  }
}

TEST(PrintBackendCupsHelperTest, TestPpdParsingSamsungPrinters) {
  {
    constexpr char kTestPpdData[] =
        R"(*PPD-Adobe: "4.3"
*ColorDevice: True
*OpenUI *ColorMode/Color Mode:  Boolean
*DefaultColorMode: True
*ColorMode False/Grayscale: ""
*ColorMode True/Color: ""
*CloseUI: *ColorMode)";

    PrinterSemanticCapsAndDefaults caps;
    EXPECT_TRUE(ParsePpdCapabilities("test", "", kTestPpdData, &caps));
    EXPECT_TRUE(caps.color_changeable);
    EXPECT_TRUE(caps.color_default);
    EXPECT_EQ(COLORMODE_COLOR, caps.color_model);
    EXPECT_EQ(COLORMODE_MONOCHROME, caps.bw_model);
  }
}

}  // namespace printing
