#ifndef _MIME_H_INCLUDED_
#define _MIME_H_INCLUDED_
/* @(#$Id: mimeparse.h,v 1.2 2005-03-25 09:40:28 dockes Exp $  (C) 2004 J.F.Dockes */

#include <string>
#include <map>

// Simple (no quoting) mime value parser. Input: "value; pn1=pv1; pn2=pv2
class MimeHeaderValue {
 public:
    std::string value;
    std::map<std::string, std::string> params;
};
extern bool parseMimeHeaderValue(const std::string& in, MimeHeaderValue& psd);

bool qp_decode(const std::string& in, std::string &out);
bool base64_decode(const std::string& in, std::string &out);

#endif /* _MIME_H_INCLUDED_ */
