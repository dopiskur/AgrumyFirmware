#ifndef ConfigParseLogic_H
#define ConfigParseLogic_H

#include <ArduinoJson.h>
#include "ConditionTree.h"

// Pure, host-testable pieces of ConfigParser::parse(), split out so the
// rule-tree cap/reject rules and the schema-version check have real native test coverage instead of
// only being exercised implicitly through a device or the (Arduino-dependent, native-untestable)
// ConfigParser.cpp itself.

// Recursively parses one JSON ConditionNode (api.Models.ConditionNode) into rule.nodes[], returning
// its index there, or -1 to reject the WHOLE rule (an unrecognized type, too many total nodes, or a
// group with zero/too-many children never stores a partial/broken tree). `rule` stays a valid
// reference across the recursive calls - rule.nodes is a fixed-size array member, never reallocated.
int parseConditionNode(JsonObject nodeJson, Rule &rule);

// Parses one JSON rule object (api.Models.Rule) into outRule - false rejects the whole rule (no root,
// or parseConditionNode above rejected it) and leaves outRule unspecified; true means outRule.rootIndex
// is set and ready to store.
bool parseRule(JsonObject r, Rule &outRule);

// True when the server's config predates fields this firmware build understands (a genuine "OTA to
// catch up" signal), never the reverse - an OLDER server schema is always fine, every field just falls
// back via ArduinoJson's own "|" default.
bool isConfigSchemaNewerThanFirmware(int serverSchemaVersion, int localSchemaVersion);

#endif
