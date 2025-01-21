<xsl:stylesheet
xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
version="2.0">
<!-- <xsl:strip-space elements="*"/> -->
	<xsl:output method="text" omit-xml-declaration="yes" indent="no"/>
	

	<!-- This recursively splits the input string by newline characters, inserting a <br/> at each point. -->
	<xsl:template name="repNL">
		<!-- this was in below param element - guess it's giving default value for when the template was matching to text().
			select="."
		-->
		<xsl:param name="pText" />

		<!-- Text up to first newline, or all text if no newline. -->
		<xsl:copy-of select="substring-before(concat($pText, '&#xA;'), '&#xA;')"/>

		<!-- Was there a newline? -->
		<xsl:if test="contains($pText, '&#xA;')">
			<!-- Insert <br/> -->
			<br />
			<!-- Recurse, passing string following the newline. -->
			<xsl:call-template name="repNL">
				<xsl:with-param name="pText" select="substring-after($pText, '&#xA;')"/>
			</xsl:call-template>
		</xsl:if>
	</xsl:template>

	<!-- Match all internal text nodes (element content) -->
	<xsl:template match="text()">
		<!-- Do we have any non-whitespace text at all? If not, output nothing. -->
		<xsl:if test="normalize-space(.)">
			<!-- Strip leading and trailing whitespace before initiating above recursive template. -->
			<xsl:call-template name="repNL">
				<xsl:with-param name="pText" select="replace(., '^\s+|\s+$', '')"/>
			</xsl:call-template>
		</xsl:if>
	</xsl:template>

    <!-- Root template -->
    <xsl:template match="/">
	<REMOVEME_TAG>
        <xsl:text>&#10;</xsl:text>
        <xsl:apply-templates/>
	</REMOVEME_TAG>
    </xsl:template>

	 <!-- Templates to match specific elements in the input XML -->
    <xsl:template match="shorttitle">
        <xsl:text>shorttitle: </xsl:text>
        <xsl:apply-templates/>
        <xsl:text>&#10;</xsl:text>
    </xsl:template>

    <xsl:template match="category">
        <xsl:text>&#10;</xsl:text>
    </xsl:template>

    <xsl:template match="description">
        <xsl:text>## </xsl:text>
        <xsl:apply-templates/>
    </xsl:template>

    <xsl:template match="imgpath">
        <xsl:text>&#10;![[</xsl:text>
        <xsl:apply-templates/>
        <xsl:text>]]&#10;</xsl:text>
    </xsl:template>

    <xsl:template match="param">
        <xsl:text>| </xsl:text>
        <xsl:apply-templates select="name"/>
        <xsl:text> | </xsl:text>
        <xsl:apply-templates select="type"/>
        <xsl:text> |&#10;</xsl:text>
    </xsl:template>

    <!-- This is a named template that is reused by both the Inputs and Outputs sections. -->
    <xsl:template name="parameters">
        <xsl:apply-templates/>
    </xsl:template>

    <xsl:template match="inputs">
        <xsl:text>&#10;## Inputs&#10;| Name | Description |&#10;| -- | -- |&#10;</xsl:text>
        <xsl:call-template name="parameters"/>
    </xsl:template>

    <xsl:template match="outputs">
        <xsl:text>&#10;## Outputs&#10;| Name | Description |&#10;| -- | -- |&#10;</xsl:text>
        <xsl:call-template name="parameters"/>
    </xsl:template>

    <xsl:template match="fulltitle">
        <xsl:text>fulltitle: </xsl:text>
        <xsl:apply-templates/>
        <xsl:text>&#10;---&#10;</xsl:text>
    </xsl:template>
	
    <xsl:template match="class_id">
        <xsl:text>class_id: </xsl:text>
        <xsl:apply-templates/>
        <xsl:text>&#10;</xsl:text>
    </xsl:template>
	
    <xsl:template match="class_name">
        <xsl:text>class_name: </xsl:text>
        <xsl:apply-templates/>
        <xsl:text>&#10;</xsl:text>
    </xsl:template>

    <xsl:template match="docs_name">
        <xsl:text>&#10;---&#10;</xsl:text>
        <xsl:text>docs_name: </xsl:text>
        <xsl:apply-templates/>
        <xsl:text>&#10;</xsl:text>
    </xsl:template>

    <!-- Unwanted elements (can use "a | b | c") -->
    <!-- <xsl:template match=" |  |  | "/> -->
</xsl:stylesheet>