/*
    Copyright (C) 2014-2016 Islog

    This file is part of Leosac.

    Leosac is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Leosac is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "core/auth/serializers/UserSerializer.hpp"
#include "core/SecurityContext.hpp"
#include "core/auth/User.hpp"
#include "tools/JSONUtils.hpp"

using namespace Leosac;
using namespace Leosac::Auth;

json UserJSONSerializer::serialize(const Auth::User &user, const SecurityContext &sc)
{
    json memberships = {};
    for (const auto &membership : user.group_memberships())
    {
        SecurityContext::ActionParam ap;
        ap.membership.membership_id = membership->id();
        if (sc.check_permission(SecurityContext::Action::MEMBERSHIP_READ, ap))
        {
            json group_info = {{"id", membership->id()},
                               {"type", "user-group-membership"}};
            memberships.push_back(group_info);
        }
    }
    json serialized = {
        {"id", user.id()},
        {"type", "user"},
        {"attributes",
         {{"version", user.odb_version()},
          {"username", user.username()},
          {"firstname", user.firstname()},
          {"lastname", user.lastname()},
          {"rank", static_cast<int>(user.rank())},
          {"validity-enabled", user.validity().is_enabled()}}},
        {"relationships", {{"memberships", {{"data", memberships}}}}}};

    SecurityContext::ActionParam ap;
    ap.user.user_id = user.id();
    if (sc.check_permission(SecurityContext::Action::USER_READ_EMAIL, ap))
    {
        serialized["attributes"]["email"] = user.email();
    }
    return serialized;
}

void UserJSONSerializer::unserialize(Auth::User &out, const json &in,
                                     const SecurityContext &sc)
{
    using namespace Leosac::JSONUtil;

    out.firstname(extract_with_default(in, "firstname", out.firstname()));
    out.lastname(extract_with_default(in, "lastname", out.lastname()));
    out.email(extract_with_default(in, "email", out.email()));
    out.password(extract_with_default(in, "password", out.password()));

    SecurityContext::ActionParam ap;
    ap.user.user_id = out.id();
    if (sc.check_permission(SecurityContext::Action::USER_UPDATE_RANK, ap))
    {
        // cast to int for json extraction to work, then back to UserRank for
        // setter to work.
        out.rank(static_cast<Auth::UserRank>(
            extract_with_default(in, "rank", static_cast<int>(out.rank()))));
    }
    if (sc.check_permission(SecurityContext::Action::USER_MANAGE_VALIDITY, ap))
    {
        auto validity = out.validity();
        validity.set_enabled(
            extract_with_default(in, "validity-enabled", validity.is_enabled()));
        out.validity(validity);
    }
}

std::string UserJSONStringSerializer::serialize(const Auth::User &in,
                                                const SecurityContext &sc)
{
    return UserJSONSerializer::serialize(in, sc).dump(4);
}

void UserJSONStringSerializer::unserialize(Auth::User &out, const std::string &in,
                                           const SecurityContext &sc)
{
    json tmp = json::parse(in);
    UserJSONSerializer::unserialize(out, tmp, sc);
}