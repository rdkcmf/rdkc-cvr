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
#include "xmlParser.h"


/*@description: XmlParser Constructor*/
XmlParser::XmlParser():doc(NULL), root(NULL) {

	RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Xml parser constructor\n", __FUNCTION__, __LINE__);
}

/*@description: XmlParser Distructor*/
XmlParser::~XmlParser() {

	RDK_LOG(RDK_LOG_INFO,"LOG.RDK.CVRPOLL","%s(%d): Xml parser destructor\n", __FUNCTION__, __LINE__);
}

/** @description: To get xml root node ptr.
 *  @param: void.
 *  @return: xml root node pointer.
 */
xmlNodePtr XmlParser::getRootNode() {
	return root;
}

/** @description: To get parsed xml.
 *  @param: xmlContentPtr: xml content to parse and it's length.
 *  @return: Error code, success/failure.
 */
int XmlParser::getParsedXml(const char* xmlContentPtr, int length) {

	//Getting Parsed XML
	if(NULL != xmlContentPtr) {
		doc = xmlReadMemory(xmlContentPtr, length, "noname.xml", NULL, 0);
		if(NULL == doc) {
			RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Error, XML File cann't be parsed.\n", __FUNCTION__, __LINE__);
			return RDKC_FAILURE;
		}

		root = xmlDocGetRootElement(doc);
		if (NULL == root) {

			RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Error, Empty File.\n", __FUNCTION__, __LINE__);
			freeXML();
			return RDKC_FAILURE;
		}
	}
	else {
		RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Error, Empty Content.\n", __FUNCTION__, __LINE__);
		freeXML();
		return RDKC_FAILURE;

	}

	return RDKC_SUCCESS;
}

/** @description: To get tagname of the node(tag).
 *  @param: node(tag) pointer.
 *  @return: name of node(tag).
 */
const char* XmlParser::getTagName(xmlNodePtr tag) {

	const char* tagName = NULL;
	if (NULL != tag) {
		tagName = (const char*)tag->name;
		RDK_LOG(RDK_LOG_DEBUG,"LOG.RDK.CVRPOLL","%s(%d): Tag Name : %s\n", __FUNCTION__, __LINE__, tagName);
	}
	else {
		RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Error, Empty Tag\n", __FUNCTION__, __LINE__);
	}

	return tagName;
}

/** @description: To get attribute value.
 *  @param: node(tag) pointer, attributeName.
 *  @return: value of the attribute.
 */
int XmlParser::getAttributeValue(xmlNodePtr tag, const char* attributeName, char* attributeValue) {

	const char* attribute_value = NULL;
	if (NULL != tag) {
		attribute_value = (const char*)xmlGetProp(tag, (const xmlChar*)attributeName);
		if(NULL == attribute_value) {
			RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Attribute[%s] not available\n", __FUNCTION__, __LINE__, attributeName);
			return RDKC_FAILURE;
		}
		else {
			strncpy(attributeValue, attribute_value, strlen(attribute_value)+1);
			RDK_LOG(RDK_LOG_DEBUG,"LOG.RDK.CVRPOLL","%s(%d): Attribute Value : %s\n", __FUNCTION__, __LINE__, attributeValue);
		}
		if(NULL != attribute_value) {
			free((char*)attribute_value);
			attribute_value = NULL;
		}
	}
	else {
		RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Error, Empty Tag\n", __FUNCTION__, __LINE__);
		return RDKC_FAILURE;
	}

	return 	RDKC_SUCCESS;
}

/** @description: To get attribute of the node(tag).
 *  @param: node(tag) pointer.
 *  @return: attribute pointer.
 */
int XmlParser::getAttributeCount(xmlNodePtr tag) {

	xmlAttrPtr attribute = NULL;
	int attributeCount = 0;
	if (NULL != tag) {
		attribute = tag->properties;
		while(attribute)
		{
			xmlChar* value = xmlNodeListGetString(tag->doc, attribute->children, 1);
			RDK_LOG(RDK_LOG_DEBUG,"LOG.RDK.CVRPOLL","%s(%d): Attributes : %s\n", __FUNCTION__, __LINE__ , value);
  			attributeCount++;
			xmlFree(value);
  			attribute = attribute->next;
		}
	}
	else {
		RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Error, Empty Tag\n", __FUNCTION__, __LINE__);
	}

	return attributeCount;

}

/** @description: To get first child of the node(tag).
 *  @param: node(tag) pointer.
 *  @return: fisrt child.
 */
xmlNodePtr XmlParser::getFirstChild(xmlNodePtr tag) {

	xmlNodePtr firstChild = NULL;
	if (NULL != tag) {
		firstChild = tag->xmlChildrenNode;
		if(NULL != firstChild) {
			RDK_LOG(RDK_LOG_DEBUG,"LOG.RDK.CVRPOLL","%s(%d): Child Name : %s\n", __FUNCTION__, __LINE__, firstChild->name);
		}
	}
	else {
		RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Error, Empty Tag\n", __FUNCTION__, __LINE__);
	}

	return firstChild;
}

/** @description: To get sibling of the node(tag).
 *  @param: node(tag) pointer.
 *  @return: sibling.
 */
xmlNodePtr XmlParser::getNextSibling(xmlNodePtr tag) {

	xmlNodePtr sibling = NULL;
	if (NULL != tag) {
		sibling = tag->next;
		if(NULL != sibling) {
			RDK_LOG(RDK_LOG_DEBUG,"LOG.RDK.CVRPOLL","%s(%d): Sibling Name : %s\n", __FUNCTION__, __LINE__, sibling->name);
		}
	}
	else {
		RDK_LOG(RDK_LOG_ERROR,"LOG.RDK.CVRPOLL","%s(%d): Error, Empty Tag\n", __FUNCTION__, __LINE__);
	}

	return sibling;
}

/** @description: To free the resources.
 *  @param: void.
 *  @return: void.
 */
void XmlParser::freeXML() {

	RDK_LOG(RDK_LOG_DEBUG,"LOG.RDK.CVRPOLL","%s(%d): Freeing the parsed XML.\n", __FUNCTION__, __LINE__);
	if(NULL != doc) {
		xmlFreeDoc(doc);
		doc = NULL;
	}
}

