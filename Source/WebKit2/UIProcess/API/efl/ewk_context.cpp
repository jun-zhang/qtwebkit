/*
 * Copyright (C) 2012 Samsung Electronics
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this program; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 */

#include "config.h"
#include "ewk_context.h"

#include "BatteryProvider.h"
#include "NetworkInfoProvider.h"
#include "VibrationProvider.h"
#include "WKAPICast.h"
#include "WKContextSoup.h"
#include "WKNumber.h"
#include "WKRetainPtr.h"
#include "WKString.h"
#include "WebContext.h"
#include "ewk_context_download_client_private.h"
#include "ewk_context_history_client_private.h"
#include "ewk_context_private.h"
#include "ewk_context_request_manager_client_private.h"
#include "ewk_cookie_manager_private.h"
#include "ewk_download_job.h"
#include "ewk_download_job_private.h"
#include <WebCore/FileSystem.h>
#include <wtf/HashMap.h>
#include <wtf/text/WTFString.h>

using namespace WebCore;
using namespace WebKit;

struct _Ewk_Url_Scheme_Handler {
    Ewk_Url_Scheme_Request_Cb callback;
    void* userData;

    _Ewk_Url_Scheme_Handler()
        : callback(0)
        , userData(0)
    { }

    _Ewk_Url_Scheme_Handler(Ewk_Url_Scheme_Request_Cb callback, void* userData)
        : callback(callback)
        , userData(userData)
    { }
};

typedef HashMap<String, _Ewk_Url_Scheme_Handler> URLSchemeHandlerMap;

struct _Ewk_Context {
    unsigned __ref; /**< the reference count of the object */
    WKRetainPtr<WKContextRef> context;

    Ewk_Cookie_Manager* cookieManager;
#if ENABLE(BATTERY_STATUS)
    RefPtr<BatteryProvider> batteryProvider;
#endif
#if ENABLE(NETWORK_INFO)
    RefPtr<NetworkInfoProvider> networkInfoProvider;
#endif
#if ENABLE(VIBRATION)
    RefPtr<VibrationProvider> vibrationProvider;
#endif
    HashMap<uint64_t, Ewk_Download_Job*> downloadJobs;

    WKRetainPtr<WKSoupRequestManagerRef> requestManager;
    URLSchemeHandlerMap urlSchemeHandlers;

    Ewk_Context_History_Client historyClient;

    _Ewk_Context(WKRetainPtr<WKContextRef> contextRef)
        : __ref(1)
        , context(contextRef)
        , cookieManager(0)
        , requestManager(WKContextGetSoupRequestManager(contextRef.get()))
        , historyClient()
    {
#if ENABLE(BATTERY_STATUS)
        batteryProvider = BatteryProvider::create(context.get());
#endif

#if ENABLE(NETWORK_INFO)
        networkInfoProvider = NetworkInfoProvider::create(context.get());
#endif

#if ENABLE(VIBRATION)
        vibrationProvider = VibrationProvider::create(context.get());
#endif

#if ENABLE(MEMORY_SAMPLER)
        static bool initializeMemorySampler = false;
        static const char environmentVariable[] = "SAMPLE_MEMORY";

        if (!initializeMemorySampler && getenv(environmentVariable)) {
            WKRetainPtr<WKDoubleRef> interval(AdoptWK, WKDoubleCreate(0.0));
            WKContextStartMemorySampler(context.get(), interval.get());
            initializeMemorySampler = true;
        }
#endif
        ewk_context_request_manager_client_attach(this);
        ewk_context_download_client_attach(this);
        ewk_context_history_client_attach(this);
    }

    ~_Ewk_Context()
    {
        if (cookieManager)
            ewk_cookie_manager_free(cookieManager);

        HashMap<uint64_t, Ewk_Download_Job*>::iterator it = downloadJobs.begin();
        HashMap<uint64_t, Ewk_Download_Job*>::iterator end = downloadJobs.end();
        for ( ; it != end; ++it)
            ewk_download_job_unref(it->value);
    }
};

Ewk_Context* ewk_context_ref(Ewk_Context* ewkContext)
{
    EINA_SAFETY_ON_NULL_RETURN_VAL(ewkContext, 0);
    ++ewkContext->__ref;

    return ewkContext;
}

void ewk_context_unref(Ewk_Context* ewkContext)
{
    EINA_SAFETY_ON_NULL_RETURN(ewkContext);
    EINA_SAFETY_ON_FALSE_RETURN(ewkContext->__ref > 0);

    if (--ewkContext->__ref)
        return;

    delete ewkContext;
}

Ewk_Cookie_Manager* ewk_context_cookie_manager_get(const Ewk_Context* ewkContext)
{
    EINA_SAFETY_ON_NULL_RETURN_VAL(ewkContext, 0);

    if (!ewkContext->cookieManager)
        const_cast<Ewk_Context*>(ewkContext)->cookieManager = ewk_cookie_manager_new(WKContextGetCookieManager(ewkContext->context.get()));

    return ewkContext->cookieManager;
}

WKContextRef ewk_context_WKContext_get(const Ewk_Context* ewkContext)
{
    return ewkContext->context.get();
}

/**
 * @internal
 * Create Ewk_Context from WKContext.
 */
Ewk_Context* ewk_context_new_from_WKContext(WKContextRef contextRef)
{
    EINA_SAFETY_ON_NULL_RETURN_VAL(contextRef, 0);

    return new Ewk_Context(contextRef);
}

/**
 * @internal
 * Registers that a new download has been requested.
 */
void ewk_context_download_job_add(Ewk_Context* ewkContext, Ewk_Download_Job* ewkDownload)
{
    EINA_SAFETY_ON_NULL_RETURN(ewkContext);
    EINA_SAFETY_ON_NULL_RETURN(ewkDownload);

    uint64_t downloadId = ewk_download_job_id_get(ewkDownload);
    if (ewkContext->downloadJobs.contains(downloadId))
        return;

    ewkContext->downloadJobs.add(downloadId, ewk_download_job_ref(ewkDownload));
}

/**
 * @internal
 * Returns the #Ewk_Download_Job with the given @a downloadId, or
 * @c 0 in case of failure.
 */
Ewk_Download_Job* ewk_context_download_job_get(const Ewk_Context* ewkContext, uint64_t downloadId)
{
    EINA_SAFETY_ON_NULL_RETURN_VAL(ewkContext, 0);

    return ewkContext->downloadJobs.get(downloadId);
}

/**
 * @internal
 * Removes the #Ewk_Download_Job with the given @a downloadId from the internal
 * HashMap.
 */
void ewk_context_download_job_remove(Ewk_Context* ewkContext, uint64_t downloadId)
{
    EINA_SAFETY_ON_NULL_RETURN(ewkContext);
    Ewk_Download_Job* download = ewkContext->downloadJobs.take(downloadId);
    if (download)
        ewk_download_job_unref(download);
}

/**
 * Retrieve the request manager for @a ewkContext.
 *
 * @param ewkContext a #Ewk_Context object.
 */
WKSoupRequestManagerRef ewk_context_request_manager_get(const Ewk_Context* ewkContext)
{
    return ewkContext->requestManager.get();
}

/**
 * @internal
 * A new URL request was received.
 *
 * @param ewkContext a #Ewk_Context object.
 * @param schemeRequest a #Ewk_Url_Scheme_Request object.
 */
void ewk_context_url_scheme_request_received(Ewk_Context* ewkContext, Ewk_Url_Scheme_Request* schemeRequest)
{
    EINA_SAFETY_ON_NULL_RETURN(ewkContext);
    EINA_SAFETY_ON_NULL_RETURN(schemeRequest);

    _Ewk_Url_Scheme_Handler handler = ewkContext->urlSchemeHandlers.get(ewk_url_scheme_request_scheme_get(schemeRequest));
    if (!handler.callback)
        return;

    handler.callback(schemeRequest, handler.userData);
}

Ewk_Context* ewk_context_default_get()
{
    static Ewk_Context defaultContext(adoptWK(WKContextCreate()));

    return &defaultContext;
}

Ewk_Context* ewk_context_new()
{
    return new Ewk_Context(adoptWK(WKContextCreate()));
}

Ewk_Context* ewk_context_new_with_injected_bundle_path(const char* path)
{
    EINA_SAFETY_ON_NULL_RETURN_VAL(path, 0);

    WKRetainPtr<WKStringRef> pathRef(AdoptWK, WKStringCreateWithUTF8CString(path));
    if (!fileExists(toImpl(pathRef.get())->string()))
        return 0;

    return new Ewk_Context(adoptWK(WKContextCreateWithInjectedBundlePath(pathRef.get())));
}

Eina_Bool ewk_context_url_scheme_register(Ewk_Context* ewkContext, const char* scheme, Ewk_Url_Scheme_Request_Cb callback, void* userData)
{
    EINA_SAFETY_ON_NULL_RETURN_VAL(ewkContext, false);
    EINA_SAFETY_ON_NULL_RETURN_VAL(scheme, false);
    EINA_SAFETY_ON_NULL_RETURN_VAL(callback, false);

    ewkContext->urlSchemeHandlers.set(String::fromUTF8(scheme), _Ewk_Url_Scheme_Handler(callback, userData));
    WKRetainPtr<WKStringRef> wkScheme(AdoptWK, WKStringCreateWithUTF8CString(scheme));
    WKSoupRequestManagerRegisterURIScheme(ewkContext->requestManager.get(), wkScheme.get());

    return true;
}

void ewk_context_vibration_client_callbacks_set(Ewk_Context* ewkContext, Ewk_Vibration_Client_Vibrate_Cb vibrate, Ewk_Vibration_Client_Vibration_Cancel_Cb cancel, void* data)
{
    EINA_SAFETY_ON_NULL_RETURN(ewkContext);

#if ENABLE(VIBRATION)
    ewkContext->vibrationProvider->setVibrationClientCallbacks(vibrate, cancel, data);
#endif
}

void ewk_context_history_callbacks_set(Ewk_Context* ewkContext, Ewk_History_Navigation_Cb navigate, Ewk_History_Client_Redirection_Cb clientRedirect, Ewk_History_Server_Redirection_Cb serverRedirect, Ewk_History_Title_Update_Cb titleUpdate, Ewk_History_Populate_Visited_Links_Cb populateVisitedLinks, void* data)
{
    EINA_SAFETY_ON_NULL_RETURN(ewkContext);

    ewkContext->historyClient.navigate_func = navigate;
    ewkContext->historyClient.client_redirect_func = clientRedirect;
    ewkContext->historyClient.server_redirect_func = serverRedirect;
    ewkContext->historyClient.title_update_func = titleUpdate;
    ewkContext->historyClient.populate_visited_links_func = populateVisitedLinks;
    ewkContext->historyClient.user_data = data;
}

const Ewk_Context_History_Client* ewk_context_history_client_get(const Ewk_Context* ewkContext)
{
    EINA_SAFETY_ON_NULL_RETURN_VAL(ewkContext, 0);

    return &ewkContext->historyClient;
}

void ewk_context_visited_link_add(Ewk_Context* ewkContext, const char* visitedURL)
{
    EINA_SAFETY_ON_NULL_RETURN(ewkContext);
    EINA_SAFETY_ON_NULL_RETURN(visitedURL);

    WKRetainPtr<WKStringRef> wkVisitedURL(AdoptWK, WKStringCreateWithUTF8CString(visitedURL));
    WKContextAddVisitedLink(ewkContext->context.get(), wkVisitedURL.get());
}