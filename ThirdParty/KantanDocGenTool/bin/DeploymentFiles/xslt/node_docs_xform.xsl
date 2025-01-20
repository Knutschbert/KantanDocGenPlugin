<?xml version="1.0" encoding="ISO-8859-1"?>
<xsl:stylesheet xmlns:xsl="http://www.w3.org/1999/XSL/Transform" version="2.0">
    <xsl:output method="text"/>

    <!-- Root template -->
    <xsl:template match="/">
	<html>
        <xsl:apply-templates/>
		</html>
    </xsl:template>

    <!-- Templates to match specific elements in the input XML -->
    <xsl:template match="shorttitle">
        # <xsl:apply-templates/>
    </xsl:template>

    <xsl:template match="category"/>

    <xsl:template match="description">
        ## Description
        <xsl:apply-templates/>
    </xsl:template>

    <xsl:template match="imgpath">
        ![<xsl:apply-templates/>]
    </xsl:template>

    <xsl:template match="param">
        | <xsl:apply-templates select="name"/> | <xsl:apply-templates select="type"/> |
    </xsl:template>

    <!-- This is a named template that is reused by both the Inputs and Outputs sections. -->
    <xsl:template name="parameters">
        <xsl:apply-templates/>
    </xsl:template>

    <xsl:template match="inputs">
        Inputs:
        | Name | Type |
        | --- | --- |
        <xsl:call-template name="parameters"/>
    </xsl:template>

    <xsl:template match="outputs">
        Outputs:
        | Name | Type |
        | --- | --- |
        <xsl:call-template name="parameters"/>
    </xsl:template>

    <!-- Unwanted elements (can use "a | b | c") -->
    <xsl:template match="fulltitle | docs_name | class_id | class_name"/>

    <!-- Match all internal text nodes (element content) -->
    <xsl:template match="text()">
        <xsl:value-of select="normalize-space(.)"/>
    </xsl:template>
</xsl:stylesheet>
