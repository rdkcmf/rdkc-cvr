/**
##########################################################################
# If not stated otherwise in this file or this component's LICENSE
# file the following copyright and licenses apply:
#
# Copyright 2019 RDK Management
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
##########################################################################
**/
#ifndef _XML_PARSER_H_
#define _XML_PARSER_H_

#include <string.h>
#include <stdlib.h>
#include <libxml/xmlmemory.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include "rdk_debug.h"

#define RDKC_FAILURE                           -1
#define RDKC_SUCCESS                            0

class XmlParser {

public:
	XmlParser();
	~XmlParser();
	int getParsedXml(const char* xmlContent, int length);
	const char* getTagName(xmlNodePtr tag);
	int getAttributeValue(xmlNodePtr tag, const char* attributeName, char* attributeValue);
	xmlNodePtr getFirstChild(xmlNodePtr tag);
	xmlNodePtr getNextSibling(xmlNodePtr tag);
	int getAttributeCount(xmlNodePtr tag);
	void freeXML();
	xmlNodePtr getRootNode();

private:
	xmlDocPtr doc;
	xmlNodePtr root;
};
#endif

