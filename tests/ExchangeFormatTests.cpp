// M57.1 -- what a file name means, and what a file actually is.
//
// This file exists because of what M57 found rather than what it added.
// "Which format is this?" was answered twice -- once in the script's `export`
// command, once in the viewer's Save As -- and the two agreed only because
// somebody kept them in step. Adding a third format to two chains is how a
// program comes to write IGES from the command line and not from the menu, and
// neither copy looks wrong on its own.

#include "Core/Export/ExchangeFormat.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

using namespace paramcad;

std::filesystem::path Scratch(const std::string& name) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "ep3d-exchange-tests";
    std::filesystem::create_directories(dir);
    return dir / name;
}

void Write(const std::filesystem::path& at, const std::string& text) {
    std::ofstream out(at, std::ios::binary);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

TEST(ExchangeFormatTest, M57_FMT_001_AnExtensionMeansOneFormatAndSaysSoOnce) {
    ExchangeFormat format = ExchangeFormat::Stl;

    ASSERT_TRUE(FormatOfName("D:/parts/housing.step").has_value());
    EXPECT_EQ(*FormatOfName("D:/parts/housing.step"), ExchangeFormat::Step);
    EXPECT_EQ(*FormatOfName("housing.STP"), ExchangeFormat::Step);
    EXPECT_EQ(*FormatOfName("bracket.iges"), ExchangeFormat::Iges);
    EXPECT_EQ(*FormatOfName("bracket.IGS"), ExchangeFormat::Iges);
    EXPECT_EQ(*FormatOfName("print.stl"), ExchangeFormat::Stl);

    // A NAME THAT NAMES NOTHING, and a name whose only dot is in a folder.
    EXPECT_FALSE(FormatOfName("housing").has_value());
    EXPECT_FALSE(FormatOfName("housing.ep3d").has_value());
    EXPECT_FALSE(FormatOfName("D:/v1.2/housing").has_value())
        << "a dot in a directory name was read as an extension";

    // A VERSIONED FOLDER WITH A REAL FILE IN IT, which is the ordinary shape of
    // a shop's drive and the case the mutation gate found nothing covering:
    // taking the FIRST dot gives ".2/housing.step" and this file stops being a
    // STEP file, on a path nobody would look at twice.
    ASSERT_TRUE(FormatOfName("D:/rev1.2/housing.step").has_value())
        << "the extension was taken from the folder rather than the file";
    EXPECT_EQ(*FormatOfName("D:/rev1.2/housing.step"), ExchangeFormat::Step);
    EXPECT_EQ(*FormatOfName("D:\\rev1.2\\bracket.igs"), ExchangeFormat::Iges);

    EXPECT_TRUE(ParseExchangeFormat("Iges", format));
    EXPECT_EQ(format, ExchangeFormat::Iges);
    EXPECT_FALSE(ParseExchangeFormat("Sat", format));
    EXPECT_EQ(format, ExchangeFormat::Iges) << "a failed parse wrote to the output anyway";
}

TEST(ExchangeFormatTest, M57_FMT_002_TheDialogFilterAndTheWriterComeFromOneList) {
    // THE FAILURE THIS RULES OUT: a format offered in the file dialog and then
    // refused by the writer, because the filter string and the branch were
    // written separately. Every extension the filter advertises has to be one
    // FormatOfName knows, and every format that can be written has to appear.
    const std::string writing = FileDialogFilter(true);
    for (const ExchangeFormat format :
         {ExchangeFormat::Step, ExchangeFormat::Iges, ExchangeFormat::Stl}) {
        for (const std::string_view extension : ExtensionsOf(format)) {
            const std::string pattern = "*" + std::string(extension);
            EXPECT_NE(writing.find(pattern), std::string::npos)
                << pattern << " can be written but is not offered: " << writing;
            ASSERT_TRUE(FormatOfName("x" + std::string(extension)).has_value()) << extension;
            EXPECT_EQ(*FormatOfName("x" + std::string(extension)), format);
        }
    }

    // AND READING IS A SHORTER LIST THAN WRITING, deliberately: STL is
    // triangles and reading one back would hand every downstream operation --
    // a fillet, a section, a dimension -- a faceted copy in place of the part.
    const std::string reading = FileDialogFilter(false);
    EXPECT_NE(reading.find("*.step"), std::string::npos);
    EXPECT_NE(reading.find("*.iges"), std::string::npos);
    EXPECT_EQ(reading.find("*.stl"), std::string::npos)
        << "the import dialog offered STL: " << reading;
    EXPECT_TRUE(CanExport(ExchangeFormat::Stl));
    EXPECT_FALSE(CanImport(ExchangeFormat::Stl));
}

TEST(ExchangeFormatTest, M57_FMT_003_TheRefusalSaysWHYRatherThanJustNo) {
    // "Use .step, .iges or .stl" is no help to somebody who just typed .stl at
    // an import prompt. They did not mistype: they asked for something this
    // program will not do, and the reason is worth one sentence.
    const std::string wrongWay = WhyNameRefused("print.stl", false);
    EXPECT_NE(wrongWay.find("STL"), std::string::npos) << wrongWay;
    EXPECT_NE(wrongWay.find("triangles"), std::string::npos) << wrongWay;

    // An unknown extension gets the list, and the list is built from the same
    // rows -- so a format added later cannot be missing from the message.
    const std::string unknown = WhyNameRefused("housing.sat", true);
    EXPECT_NE(unknown.find(".step"), std::string::npos) << unknown;
    EXPECT_NE(unknown.find(".iges"), std::string::npos) << unknown;
    EXPECT_NE(unknown.find(".stl"), std::string::npos) << unknown;
    // ...and the reading list leaves STL out of the suggestion, because
    // suggesting it would be suggesting the thing just refused.
    const std::string unknownRead = WhyNameRefused("housing.sat", false);
    EXPECT_EQ(unknownRead.find(".stl"), std::string::npos) << unknownRead;
}

TEST(ExchangeFormatTest, M57_FMT_004_WhatAFileISBeatsWhatItIsCALLED) {
    // THE CASE THIS EXISTS FOR. Exchange files are renamed more than any other
    // kind in a shop: a supplier sends housing.stp that is IGES inside. A
    // reader that trusted the name would report a STEP syntax error about a
    // perfectly good IGES file, and the reader then goes looking for a fault
    // in the file rather than in the name.
    const std::filesystem::path lying = Scratch("housing.stp");
    Write(lying,
          "                                                                        S      1\n"
          "1H,,1H;,7Hhousing,                                                      G      1\n");
    ASSERT_TRUE(FormatOfContents(lying.string()).has_value());
    EXPECT_EQ(*FormatOfContents(lying.string()), ExchangeFormat::Iges)
        << "an IGES file called .stp was taken for STEP";
    // The NAME still says STEP, which is exactly the disagreement.
    EXPECT_EQ(*FormatOfName(lying.string()), ExchangeFormat::Step);

    const std::filesystem::path honest = Scratch("bracket.igs");
    Write(honest, "ISO-10303-21;\nHEADER;\nFILE_DESCRIPTION((''),'2;1');\n");
    ASSERT_TRUE(FormatOfContents(honest.string()).has_value());
    EXPECT_EQ(*FormatOfContents(honest.string()), ExchangeFormat::Step);

    // An ASCII STL is recognised too -- not so it can be read, but so that a
    // user who tries is told what their file IS.
    const std::filesystem::path mesh = Scratch("print.stl");
    Write(mesh, "solid part\n facet normal 0 0 1\n");
    ASSERT_TRUE(FormatOfContents(mesh.string()).has_value());
    EXPECT_EQ(*FormatOfContents(mesh.string()), ExchangeFormat::Stl);

    // Nothing, for a file that is none of these and for one that is not there.
    const std::filesystem::path other = Scratch("notes.txt");
    Write(other, "this is not a CAD file at all\n");
    EXPECT_FALSE(FormatOfContents(other.string()).has_value());
    EXPECT_FALSE(FormatOfContents(Scratch("missing.step").string()).has_value());

    // AN EMPTY FILE IS NOT A FORMAT. It is the shape a failed export leaves
    // behind, and calling it STEP would send a reader to the wrong question.
    const std::filesystem::path empty = Scratch("empty.step");
    Write(empty, "");
    EXPECT_FALSE(FormatOfContents(empty.string()).has_value());
}

TEST(ExchangeFormatTest, M57_FMT_005_AnIgesCardIsRecognisedByItsSHAPEAndNotAMagicNumber) {
    // IGES has no magic number. It is a card format: every line is 80 columns
    // and column 73 carries the section letter. So the shape of the record IS
    // the identification, and a line that merely CONTAINS an S is not one.
    const std::filesystem::path notIges = Scratch("prose.txt");
    Write(notIges, "S is the first letter of this sentence, which is not 80 columns long.\n");
    EXPECT_FALSE(FormatOfContents(notIges.string()).has_value());

    // A file whose section letter sits at column 73 of a long enough line is,
    // even without a start section before it -- because a compressed IGES
    // begins with an F line and a Global-only fragment begins with G.
    const std::filesystem::path global = Scratch("fragment.igs");
    Write(global, std::string(72, ' ') + "G      1\n");
    ASSERT_TRUE(FormatOfContents(global.string()).has_value());
    EXPECT_EQ(*FormatOfContents(global.string()), ExchangeFormat::Iges);

    // ...and a line of exactly 72 characters is one short, so it is not.
    const std::filesystem::path shortLine = Scratch("short.igs");
    Write(shortLine, std::string(72, ' ') + "\n");
    EXPECT_FALSE(FormatOfContents(shortLine.string()).has_value());

    // CARRIAGE RETURNS DO NOT COUNT AS A COLUMN. A file written on Windows and
    // read on either has the same 80 columns, and a check that measured the
    // line including its \\r would find the section letter one place early.
    const std::filesystem::path crlf = Scratch("crlf.igs");
    Write(crlf, std::string(72, ' ') + "S      1\r\n");
    ASSERT_TRUE(FormatOfContents(crlf.string()).has_value())
        << "a CRLF line moved the section column";
    EXPECT_EQ(*FormatOfContents(crlf.string()), ExchangeFormat::Iges);
}

} // namespace
