// Copyright (c) Microsoft Corporation. All rights reserved.
//
// Licensed under the MIT License. See LICENSE.txt in the project root for license information.

#include "pch.h"

#include <lib/printing/CanvasPreviewEventArgs.h>
#include <lib/printing/CanvasPrintDocument.h>
#include <lib/printing/CanvasPrintDocumentAdapter.h>
#include <lib/printing/CanvasPrintEventArgs.h>
#include <lib/printing/CanvasPrintTaskOptionsChangedEventArgs.h>

#include "../mocks/MockPrintControl.h"
#include "../mocks/MockPrintDocumentPackageTarget.h"
#include "../mocks/MockPrintPreviewDxgiPackageTarget.h"
#include "../mocks/MockPrintTaskOptions.h"

using namespace ABI::Microsoft::Graphics::Canvas::Printing;


static uint32_t const AnyPageNumber = 123;
static float const AnyWidth = 12.0f;
static float const AnyHeight = 34.0f;
static float const AnyDpi = 120.0f;

class TestPrintDocumentAdapter : public CanvasPrintDocumentAdapter
{
public:
    ComPtr<StubDispatcher> Dispatcher;
    ComPtr<MockCanvasDevice> SharedDevice;
    float Dpi;
    
    TestPrintDocumentAdapter()
        : Dispatcher(Make<StubDispatcher>())
        , SharedDevice(Make<MockCanvasDevice>())
        , Dpi(AnyDpi)
    {
        SharedDevice->CreateRenderTargetBitmapMethod.AllowAnyCall(
            [&] (float, float, float, DirectXPixelFormat, CanvasAlphaMode)
            {
                return Make<StubD2DBitmap>(D2D1_BITMAP_OPTIONS_TARGET);
            });

        SharedDevice->CreateDeviceContextForDrawingSessionMethod.AllowAnyCall(
            [=]
            {
                return Make<StubD2DDeviceContext>(nullptr);
            });
    }

    virtual ComPtr<ICanvasDevice> GetSharedDevice() override
    {
        return SharedDevice;
    }

    virtual ComPtr<ICoreDispatcher> GetDispatcherForCurrentThread() override
    {
        return Dispatcher;
    }

    virtual float GetLogicalDpi() override
    {
        return Dpi;
    }

    void RunNextAction()
    {
        Dispatcher->Tick();
    }
};


TEST_CLASS(CanvasPrintDocumentUnitTests)
{
    struct Fixture
    {
        std::shared_ptr<TestPrintDocumentAdapter> Adapter;
        ComPtr<CanvasPrintDocumentFactory> Factory;

        ComPtr<MockPrintTaskOptions> AnyPrintTaskOptions;

        Fixture()
            : AnyPrintTaskOptions(Make<MockPrintTaskOptions>())
        {
            Adapter = std::make_shared<TestPrintDocumentAdapter>();
            CanvasPrintDocumentAdapter::SetInstance(Adapter);

            Factory = Make<CanvasPrintDocumentFactory>();

            AnyPrintTaskOptions->GetPageDescriptionMethod.AllowAnyCall();
        }

        ComPtr<ICanvasPrintDocument> Create()
        {
            ComPtr<IInspectable> docInsp;
            ThrowIfFailed(Factory->ActivateInstance(&docInsp));
            
            return As<ICanvasPrintDocument>(docInsp);
        }
    };

    TEST_METHOD_EX(CanvasPrintDocument_DefaultActivation_FailsWhenNoDispatcher)
    {
        Fixture f;
        f.Adapter->Dispatcher.Reset();

        ComPtr<IInspectable> docInsp;
        Assert::AreEqual(RPC_E_WRONG_THREAD, f.Factory->ActivateInstance(&docInsp));
        ValidateStoredErrorState(RPC_E_WRONG_THREAD, Strings::CanvasPrintDocumentMustBeConstructedOnUIThread);
    }

    TEST_METHOD_EX(CanvasPrintDocument_DefaultActivation_UsesSharedDevice)
    {
        Fixture f;

        auto doc = As<ICanvasResourceCreator>(f.Create());

        ComPtr<ICanvasDevice> retrievedDevice;
        ThrowIfFailed(doc->get_Device(&retrievedDevice));

        Assert::IsTrue(IsSameInstance(f.Adapter->SharedDevice.Get(), retrievedDevice.Get()));        
    }

    TEST_METHOD_EX(CanvasPrintDocument_CreateWithDevice_UsesProvidedDevice)
    {
        Fixture f;

        auto device = Make<MockCanvasDevice>();

        ComPtr<ICanvasPrintDocument> doc;
        ThrowIfFailed(f.Factory->CreateWithDevice(device.Get(), &doc));

        ComPtr<ICanvasDevice> retrievedDevice;
        ThrowIfFailed(As<ICanvasResourceCreator>(doc)->get_Device(&retrievedDevice));

        Assert::IsTrue(IsSameInstance(device.Get(), retrievedDevice.Get()));
    }

    TEST_METHOD_EX(CanvasPrintDocument_CreateWithDevice_FailsIfPassedInvalidParams)        
    {
        Fixture f;
        
        auto device = Make<MockCanvasDevice>();
        ComPtr<ICanvasPrintDocument> doc;

        Assert::AreEqual(E_INVALIDARG, f.Factory->CreateWithDevice(nullptr, &doc));
        Assert::AreEqual(E_INVALIDARG, f.Factory->CreateWithDevice(device.Get(), nullptr));
    }

    TEST_METHOD_EX(CanvasPrintDocument_GetPreviewPageCollection_FailsIfPassedInvalidParams)
    {
        Fixture f;

        auto doc = As<IPrintDocumentPageSource>(f.Create());

        ComPtr<IPrintPreviewPageCollection> c;

        Assert::AreEqual(E_INVALIDARG, doc->GetPreviewPageCollection(nullptr, &c));
        Assert::AreEqual(E_INVALIDARG, doc->GetPreviewPageCollection(Make<MockPrintDocumentPackageTarget>().Get(), nullptr));
    }

    struct PrintPreviewFixture : public Fixture
    {
        ComPtr<MockPrintPreviewDxgiPackageTarget> PreviewTarget;

        ComPtr<ICanvasPrintDocument> Doc;
        ComPtr<IPrintPreviewPageCollection> PageCollection;

        MockEventHandler<CanvasPrintTaskOptionsChangedHandler> PrintTaskOptionsChangedHandler;
        MockEventHandler<CanvasPreviewHandler> PreviewHandler;
        
        PrintPreviewFixture()
            : Doc(Create())
            , PreviewTarget(Make<MockPrintPreviewDxgiPackageTarget>())
            , PrintTaskOptionsChangedHandler(L"PrintTaskOptionsChangedHandler")
            , PreviewHandler(L"PreviewHandler")
        {            
            PreviewTarget->DrawPageMethod.AllowAnyCall();

            auto target = Make<MockPrintDocumentPackageTarget>();
            target->GetPackageTargetMethod.SetExpectedCalls(1,
                [=] (const GUID& type, const IID& iid, void** ppv)
                {
                    Assert::AreEqual(ID_PREVIEWPACKAGETARGET_DXGI, type);
                    return PreviewTarget.CopyTo(iid, ppv);
                });

            ThrowIfFailed(As<IPrintDocumentPageSource>(Doc)->GetPreviewPageCollection(target.Get(), &PageCollection));
        }

        EventRegistrationToken RegisterPrintTaskOptionsChanged()
        {
            EventRegistrationToken token;
            ThrowIfFailed(Doc->add_PrintTaskOptionsChanged(PrintTaskOptionsChangedHandler.Get(), &token));
            return token;
        }

        EventRegistrationToken RegisterPreview()
        {
            EventRegistrationToken token;
            ThrowIfFailed(Doc->add_Preview(PreviewHandler.Get(), &token));
            return token;
        }
    };

    TEST_METHOD_EX(CanvasPrintDocument_GetPreviewPageCollection_ReturnsCollection)
    {
        PrintPreviewFixture f;
        Assert::IsTrue(f.PageCollection);
    }

    TEST_METHOD_EX(CanvasPrintDocument_When_InvalidatePreview_CalledBeforePreviewing_ItIsNoOp)
    {
        Fixture f;
        auto doc = f.Create();

        ThrowIfFailed(doc->InvalidatePreview());
    }

    TEST_METHOD_EX(CanvasPrintDocument_InvalidatePreview_ForwardsToPreviewTarget)
    {
        PrintPreviewFixture f;

        f.PreviewTarget->InvalidatePreviewMethod.SetExpectedCalls(1);
        ThrowIfFailed(f.Doc->InvalidatePreview());
    }

    TEST_METHOD_EX(CanvasPrintDocument_When_SetPageCount_CalledBeforePreviewing_ItFails)
    {
        Fixture f;
        auto doc = f.Create();

        Assert::AreEqual(E_FAIL, doc->SetPageCount(1));
        ValidateStoredErrorState(E_FAIL, Strings::SetPageCountCalledBeforePreviewing);
    }

    TEST_METHOD_EX(CanvasPrintDocument_When_SetIntermediatePageCount_CalledBeforePreviewing_ItFails)
    {
        Fixture f;
        auto doc = f.Create();

        Assert::AreEqual(E_FAIL, doc->SetIntermediatePageCount(1));
        ValidateStoredErrorState(E_FAIL, Strings::SetPageCountCalledBeforePreviewing);
    }

    TEST_METHOD_EX(CanvasPrintDocument_SetPageCount_ForwardsToPreviewTarget)
    {
        PrintPreviewFixture f;

        f.PreviewTarget->SetJobPageCountMethod.SetExpectedCalls(1,
            [&] (PageCountType t, UINT32 c)
            {
                Assert::AreEqual<int>(PageCountType::FinalPageCount, t);
                Assert::AreEqual(c, (uint32_t)AnyPageNumber);
                return S_OK;
            });
        ThrowIfFailed(f.Doc->SetPageCount(AnyPageNumber));
    }

    TEST_METHOD_EX(CanvasPrintDocument_SetIntermediatePageCount_ForwardsToPreviewTarget)
    {
        PrintPreviewFixture f;

        f.PreviewTarget->SetJobPageCountMethod.SetExpectedCalls(1,
            [&] (PageCountType t, UINT32 c)
            {
                Assert::AreEqual<int>(PageCountType::IntermediatePageCount, t);
                Assert::AreEqual(c, (uint32_t)AnyPageNumber);
                return S_OK;
            });
        ThrowIfFailed(f.Doc->SetIntermediatePageCount(AnyPageNumber));
    }

    TEST_METHOD_EX(CanvasPrintDocument_WhenPaginateCalled_PrintTaskOptionsChangedIsRaised)
    {
        PrintPreviewFixture f;

        f.RegisterPrintTaskOptionsChanged();

        // The call to Paginate is meant to just queue up the work, so we don't
        // expect the event to be raised until we call RunNextAction.
        ThrowIfFailed(f.PageCollection->Paginate(static_cast<uint32_t>(AnyPageNumber), f.AnyPrintTaskOptions.Get()));

        f.PrintTaskOptionsChangedHandler.SetExpectedCalls(1,
            [&] (ICanvasPrintDocument* sender, ICanvasPrintTaskOptionsChangedEventArgs* args)
            {
                Assert::IsTrue(IsSameInstance(sender, f.Doc.Get()));
                Assert::IsNotNull(args);

                uint32_t currentPreviewPageNumber = 0;
                ThrowIfFailed(args->get_CurrentPreviewPageNumber(&currentPreviewPageNumber));
                Assert::AreEqual(AnyPageNumber, currentPreviewPageNumber);

                uint32_t newPreviewPageNumber = 0;
                ThrowIfFailed(args->get_NewPreviewPageNumber(&newPreviewPageNumber));
                Assert::AreEqual(1U, newPreviewPageNumber);

                ComPtr<IPrintTaskOptionsCore> retrievedPrintTaskOptions;
                ThrowIfFailed(args->get_PrintTaskOptions(&retrievedPrintTaskOptions));
                Assert::IsTrue(IsSameInstance(f.AnyPrintTaskOptions.Get(), retrievedPrintTaskOptions.Get()));
                
                return S_OK;
            });

        f.Adapter->RunNextAction();
    }

    TEST_METHOD_EX(CanvasPrintDocument_WhenPaginateCalled_WithPageSetToMinusOne_PrintTaskOptionsIsRaisedWithCurrentPageSetToOne)
    {
        PrintPreviewFixture f;
        
        f.RegisterPrintTaskOptionsChanged();

        //
        // The print system will call Paginate with JOB_PAGE_APPLICATION_DEFINED
        // if this is the first time it has been called (and so no page is
        // currently displayed).
        //
        // Win2D massages this to be '1' in this case.
        //
        ThrowIfFailed(f.PageCollection->Paginate(JOB_PAGE_APPLICATION_DEFINED, f.AnyPrintTaskOptions.Get()));

        f.PrintTaskOptionsChangedHandler.SetExpectedCalls(1,
            [&] (ICanvasPrintDocument* sender, ICanvasPrintTaskOptionsChangedEventArgs* args)
            {
                uint32_t currentPreviewPageNumber = 0;
                ThrowIfFailed(args->get_CurrentPreviewPageNumber(&currentPreviewPageNumber));
                Assert::AreEqual(1U, currentPreviewPageNumber);
                return S_OK;
            });

        f.Adapter->RunNextAction();
    }

    TEST_METHOD_EX(CanvasPrintDocument_WhenPrintTaskOptionsChangedIsUnregistered_ItIsNotCalled)
    {
        PrintPreviewFixture f;

        auto token = f.RegisterPrintTaskOptionsChanged();
        ThrowIfFailed(f.Doc->remove_PrintTaskOptionsChanged(token));

        ThrowIfFailed(f.PageCollection->Paginate(AnyPageNumber, f.AnyPrintTaskOptions.Get()));
        f.Adapter->RunNextAction();        
    }

    TEST_METHOD_EX(CanvasPrintDocument_add_PrintTaskOptionsChanged_FailsWithBadParams)
    {
        PrintPreviewFixture f;

        EventRegistrationToken token;
        Assert::AreEqual(E_INVALIDARG, f.Doc->add_PrintTaskOptionsChanged(nullptr, &token));
        Assert::AreEqual(E_INVALIDARG, f.Doc->add_PrintTaskOptionsChanged(f.PrintTaskOptionsChangedHandler.Get(), nullptr));
    }

    TEST_METHOD_EX(CanvasPrintDocument_WhenMakePageCalled_PreviewIsRaised)
    {
        PrintPreviewFixture f;

        f.RegisterPreview();

        // The system will always call Paginate before MakePage.
        ThrowIfFailed(f.PageCollection->Paginate(AnyPageNumber, f.AnyPrintTaskOptions.Get()));
        f.Adapter->RunNextAction();
        
        ThrowIfFailed(f.PageCollection->MakePage(AnyPageNumber, AnyWidth, AnyHeight));

        f.PreviewHandler.SetExpectedCalls(1,
            [&] (ICanvasPrintDocument* sender, ICanvasPreviewEventArgs* args)
            {
                Assert::IsTrue(IsSameInstance(f.Doc.Get(), sender));
                Assert::IsNotNull(args);

                uint32_t pageNumber;
                ThrowIfFailed(args->get_PageNumber(&pageNumber));
                Assert::AreEqual(AnyPageNumber, pageNumber);

                ComPtr<IPrintTaskOptionsCore> retrievedPrintTaskOptions;
                ThrowIfFailed(args->get_PrintTaskOptions(&retrievedPrintTaskOptions));
                Assert::IsTrue(IsSameInstance(f.AnyPrintTaskOptions.Get(), retrievedPrintTaskOptions.Get()));

                return S_OK;
            });

        f.Adapter->RunNextAction();
    }

    TEST_METHOD_EX(CanvasPrintDocument_WhenMakePageCalled_WithJobPageAppDefined_NewPreviewPageNumberUsed)
    {
        PrintPreviewFixture f;

        f.RegisterPrintTaskOptionsChanged();
        f.RegisterPreview();
        
        ThrowIfFailed(f.PageCollection->Paginate(JOB_PAGE_APPLICATION_DEFINED, f.AnyPrintTaskOptions.Get()));

        f.PrintTaskOptionsChangedHandler.SetExpectedCalls(1,
            [&] (ICanvasPrintDocument* sender, ICanvasPrintTaskOptionsChangedEventArgs* args)
            {
                ThrowIfFailed(args->put_NewPreviewPageNumber(AnyPageNumber));
                return S_OK;
            });

        f.Adapter->RunNextAction();
        
        ThrowIfFailed(f.PageCollection->MakePage(JOB_PAGE_APPLICATION_DEFINED, AnyWidth, AnyHeight));

        f.PreviewHandler.SetExpectedCalls(1,
            [&] (ICanvasPrintDocument* sender, ICanvasPreviewEventArgs* args)
            {
                uint32_t pageNumber;
                ThrowIfFailed(args->get_PageNumber(&pageNumber));
                Assert::AreEqual(AnyPageNumber, pageNumber);
                return S_OK;
            });

        f.Adapter->RunNextAction();        
    }

    TEST_METHOD_EX(CanvasPrintDocument_WhenMakePageCalled_PreviewIsDrawn)
    {
        // To draw the preview:
        //
        // - a DXGI surface of the correct size must be created
        //
        // - preview handler must be called with a drawing session,
        //   appropriately configured
        //
        // - IPrintPreviewDxgiPackageTarget::DrawPage() must be called with the
        //   DXGI surface and the correct DPI values

        PrintPreviewFixture f;
        f.RegisterPreview();

        float pageWidth = 100;
        float pageHeight = 200;

        PrintPageDescription printPageDescription {
            Size{ pageWidth, pageHeight },
            Rect{ 0, 0, pageWidth, pageHeight }, // ImageableRect
            (uint32_t)AnyDpi, (uint32_t)AnyDpi   // DPI X, Y
        };

        auto printTaskOptions = Make<MockPrintTaskOptions>();
        printTaskOptions->GetPageDescriptionMethod.SetExpectedCalls(1,
            [&] (uint32_t page, PrintPageDescription* outDesc)
            {
                Assert::AreEqual(AnyPageNumber, page);
                *outDesc = printPageDescription;
                return S_OK;
            });

        ThrowIfFailed(f.PageCollection->Paginate(static_cast<uint32_t>(AnyPageNumber), printTaskOptions.Get()));
        f.Adapter->RunNextAction();

        
        float previewScale = 0.5f;

        float displayWidth = pageWidth * previewScale;
        float displayHeight = pageHeight * previewScale;

        ThrowIfFailed(f.PageCollection->MakePage(AnyPageNumber, displayWidth, displayHeight));

        float expectedBitmapDpi = f.Adapter->Dpi * previewScale;

        auto d2dBitmap = Make<StubD2DBitmap>(D2D1_BITMAP_OPTIONS_TARGET);

        f.Adapter->SharedDevice->CreateRenderTargetBitmapMethod.SetExpectedCalls(1,
            [&] (float width, float height, float dpi, DirectXPixelFormat format, CanvasAlphaMode alpha)
            {
                // The width/height of the RT should be the same as the
                // width/height of the page (since we've adjusted the DPI so
                // that pageSize * DPI = previewSizeInPixels.
                Assert::AreEqual(pageWidth, width);
                Assert::AreEqual(pageHeight, height);

                Assert::AreEqual(expectedBitmapDpi, dpi);

                Assert::AreEqual(PIXEL_FORMAT(B8G8R8A8UIntNormalized), format);
                Assert::AreEqual(CanvasAlphaMode::Premultiplied, alpha);

                return d2dBitmap;
            });

        f.PreviewHandler.SetExpectedCalls(1,
            [&] (ICanvasPrintDocument*, ICanvasPreviewEventArgs* args)
            {
                ComPtr<ICanvasDrawingSession> ds;
                ThrowIfFailed(args->get_DrawingSession(&ds));

                // This drawing session should be pointing at the render target
                // that was created
                auto deviceContext = GetWrappedResource<ID2D1DeviceContext>(ds);
                ComPtr<ID2D1Image> currentTarget;
                deviceContext->GetTarget(&currentTarget);
                Assert::IsTrue(IsSameInstance(d2dBitmap.Get(), currentTarget.Get()));
                
                return S_OK;
            });

        f.PreviewTarget->DrawPageMethod.SetExpectedCalls(1,
            [&] (uint32_t pageNumber, IDXGISurface* dxgiSurface, float dpiX, float dpiY)
            {
                Assert::AreEqual<int>(AnyPageNumber, pageNumber);
                Assert::AreEqual(expectedBitmapDpi, dpiX);
                Assert::AreEqual(expectedBitmapDpi, dpiY);

                ComPtr<IDXGISurface> expectedDxgiSurface;
                ThrowIfFailed(d2dBitmap->GetSurface(&expectedDxgiSurface));
                Assert::IsTrue(IsSameInstance(expectedDxgiSurface.Get(), dxgiSurface));

                return S_OK;
            });

        f.Adapter->RunNextAction();        
    }

    TEST_METHOD_EX(CanvasPrintDocument_WhenPreviewIsUnregistered_ItIsNotCalled)
    {
        PrintPreviewFixture f;

        auto token = f.RegisterPreview();
        ThrowIfFailed(f.Doc->remove_Preview(token));

        ThrowIfFailed(f.PageCollection->Paginate(AnyPageNumber, f.AnyPrintTaskOptions.Get()));
        f.Adapter->RunNextAction();

        ThrowIfFailed(f.PageCollection->MakePage(AnyPageNumber, AnyWidth, AnyHeight));
        f.Adapter->RunNextAction();
    }

    TEST_METHOD_EX(CanvasPrintDocument_add_Preview_FailsWithBadParams)
    {
        PrintPreviewFixture f;

        EventRegistrationToken token;
        Assert::AreEqual(E_INVALIDARG, f.Doc->add_Preview(nullptr, &token));
        Assert::AreEqual(E_INVALIDARG, f.Doc->add_Preview(f.PreviewHandler.Get(), nullptr));
    }


    struct PrintFixture : public Fixture
    {
        ComPtr<ICanvasPrintDocument> Doc;
        
        MockEventHandler<CanvasPrintHandler> PrintHandler;

        ComPtr<MockPrintDocumentPackageTarget> AnyTarget;
        ComPtr<MockPrintControl> PrintControl;

        PrintFixture()
            : Doc(Create())
            , PrintHandler(L"PrintHandler")
            , AnyTarget(Make<MockPrintDocumentPackageTarget>())
            , PrintControl(Make<MockPrintControl>())
        {
            Adapter->SharedDevice->CreateCommandListMethod.AllowAnyCall(
                [=]
                {
                    auto cl = Make<MockD2DCommandList>();
                    cl->CloseMethod.SetExpectedCalls(1);
                    return cl;
                });
        }

        EventRegistrationToken RegisterPrint()
        {
            EventRegistrationToken token;
            ThrowIfFailed(Doc->add_Print(PrintHandler.Get(), &token));
            return token;
        }
    };

    TEST_METHOD_EX(CanvasPrintDocument_WhenMakeDocumentCalled_PrintEventIsRaised)
    {
        PrintFixture f;
        f.RegisterPrint();

        ThrowIfFailed(As<IPrintDocumentPageSource>(f.Doc)->MakeDocument(
            f.AnyPrintTaskOptions.Get(),
            f.AnyTarget.Get()));

        f.PrintHandler.SetExpectedCalls(1,
            [&] (ICanvasPrintDocument* doc, ICanvasPrintEventArgs* args)
            {
                Assert::IsTrue(IsSameInstance(f.Doc.Get(), doc));

                ComPtr<IPrintTaskOptionsCore> retrievedPrintTaskOptions;
                ThrowIfFailed(args->get_PrintTaskOptions(&retrievedPrintTaskOptions));
                Assert::IsTrue(IsSameInstance(f.AnyPrintTaskOptions.Get(), retrievedPrintTaskOptions.Get()));
                
                return S_OK;
            });

        f.Adapter->RunNextAction();        
    }

    TEST_METHOD_EX(CanvasPrintDocument_PrintEvent_InitialDpiValueMatchesFirstPageDpi)
    {
        PrintFixture f;
        f.RegisterPrint();

        PrintPageDescription printPageDescription {
            Size{ AnyWidth, AnyHeight },
            Rect{ 0, 0, AnyWidth, AnyHeight }, // ImageableRect
            (uint32_t)AnyDpi, (uint32_t)AnyDpi // DPI X, Y
        };

        auto printTaskOptions = Make<MockPrintTaskOptions>();
        printTaskOptions->GetPageDescriptionMethod.SetExpectedCalls(1,
            [&] (uint32_t page, PrintPageDescription* outDesc)
            {
                Assert::AreEqual(1U, page);
                *outDesc = printPageDescription;
                return S_OK;
            });

        ThrowIfFailed(As<IPrintDocumentPageSource>(f.Doc)->MakeDocument(
            printTaskOptions.Get(),
            f.AnyTarget.Get()));

        f.PrintHandler.SetExpectedCalls(1,
            [&] (ICanvasPrintDocument*, ICanvasPrintEventArgs* args)
            {
                float dpi;
                ThrowIfFailed(args->get_Dpi(&dpi));

                Assert::AreEqual(AnyDpi, dpi);
                return S_OK;
            });

        f.Adapter->RunNextAction();        
    }

    TEST_METHOD_EX(CanvasPrintDocument_PrintEvent_CreateDrawingSession_CreatesPrintControlAndClosesItWhenDone)
    {
        //
        // This test verifies that the CanvasPrintEventArgs is hooked up to the
        // right CanvasDevice / IPrintDocumentPackageTarget.  The
        // CanvasPrintEventArgsUnitTests below exercise more of the interactions
        // with these.
        //
        
        PrintFixture f;
        f.RegisterPrint();
        
        ThrowIfFailed(As<IPrintDocumentPageSource>(f.Doc)->MakeDocument(
            f.AnyPrintTaskOptions.Get(),
            f.AnyTarget.Get()));

        f.PrintHandler.SetExpectedCalls(1,
            [&] (ICanvasPrintDocument*, ICanvasPrintEventArgs* args)
            {
                f.Adapter->SharedDevice->CreatePrintControlMethod.SetExpectedCalls(1,
                    [&] (IPrintDocumentPackageTarget* target, float)
                    {
                        Assert::IsTrue(IsSameInstance(f.AnyTarget.Get(), target));
                        return f.PrintControl;
                    });

                ComPtr<ICanvasDrawingSession> ds;
                ThrowIfFailed(args->CreateDrawingSession(&ds));

                f.PrintControl->AddPageMethod.SetExpectedCalls(1);
                f.PrintControl->CloseMethod.SetExpectedCalls(1);

                return S_OK;
            });

        f.Adapter->RunNextAction();        
    }

    TEST_METHOD_EX(CanvasPrintDocument_PrintIsUnregistered_ItIsNotCalled)
    {
        PrintFixture f;

        auto token = f.RegisterPrint();
        ThrowIfFailed(f.Doc->remove_Print(token));

        ThrowIfFailed(As<IPrintDocumentPageSource>(f.Doc)->MakeDocument(
            f.AnyPrintTaskOptions.Get(),
            f.AnyTarget.Get()));
        
        f.Adapter->RunNextAction();        
    }

    TEST_METHOD_EX(CanvasPrintDocument_add_Print_FailsWithBadParams)
    {
        PrintFixture f;

        EventRegistrationToken token;
        Assert::AreEqual(E_INVALIDARG, f.Doc->add_Print(nullptr, &token));
        Assert::AreEqual(E_INVALIDARG, f.Doc->add_Print(f.PrintHandler.Get(), nullptr));
    }
};


TEST_CLASS(CanvasPrintTaskOptionsChangedEventArgsUnitTests)
{
    struct Fixture
    {
        ComPtr<MockPrintTaskOptions> AnyPrintTaskOptions;

        ComPtr<CanvasPrintTaskOptionsChangedEventArgs> Args;

        Fixture()
            : AnyPrintTaskOptions(Make<MockPrintTaskOptions>())
            , Args(Make<CanvasPrintTaskOptionsChangedEventArgs>(AnyPageNumber, AnyPrintTaskOptions))
        {
        }
    };

    TEST_METHOD_EX(CanvasPrintTaskOptionsChangedEventArgs_Getters_FailWithBadParams)
    {
        Fixture f;
        Assert::AreEqual(E_INVALIDARG, f.Args->get_CurrentPreviewPageNumber(nullptr));
        Assert::AreEqual(E_INVALIDARG, f.Args->get_NewPreviewPageNumber(nullptr));
        Assert::AreEqual(E_INVALIDARG, f.Args->GetDeferral(nullptr));
        Assert::AreEqual(E_INVALIDARG, f.Args->get_PrintTaskOptions(nullptr));
    }

    TEST_METHOD_EX(CanvasPrintTaskOptionsChangedEventArgs_NewPreviewPageNumber_MustBeGreaterThanOrEqualToOne)
    {
        Fixture f;

        Assert::AreEqual(E_INVALIDARG, f.Args->put_NewPreviewPageNumber(0));
        Assert::AreEqual(S_OK, f.Args->put_NewPreviewPageNumber(1));
        Assert::AreEqual(S_OK, f.Args->put_NewPreviewPageNumber(10));
    }
};


TEST_CLASS(CanvasPreviewEventArgsUnitTests)
{
    struct Fixture
    {
        ComPtr<MockPrintTaskOptions> AnyPrintTaskOptions;
        ComPtr<MockCanvasDrawingSession> AnyDrawingSession;

        ComPtr<CanvasPreviewEventArgs> Args;

        Fixture()
            : AnyPrintTaskOptions(Make<MockPrintTaskOptions>())
            , AnyDrawingSession(Make<MockCanvasDrawingSession>())
            , Args(Make<CanvasPreviewEventArgs>(AnyPageNumber, AnyPrintTaskOptions, AnyDrawingSession))
        {
        }
    };

    TEST_METHOD_EX(CanvasPreviewEventArgs_Getters_FailWithBadParams)
    {
        Fixture f;
        
        Assert::AreEqual(E_INVALIDARG, f.Args->get_PageNumber(nullptr));
        Assert::AreEqual(E_INVALIDARG, f.Args->get_PrintTaskOptions(nullptr));
        Assert::AreEqual(E_INVALIDARG, f.Args->GetDeferral(nullptr));
        Assert::AreEqual(E_INVALIDARG, f.Args->get_DrawingSession(nullptr));
    }
};


TEST_CLASS(CanvasPrintEventArgsUnitTests)
{
    struct Fixture
    {
        ComPtr<MockCanvasDevice> Device;
        ComPtr<MockPrintTaskOptions> PrintTaskOptions;
        ComPtr<MockPrintDocumentPackageTarget> AnyTarget;
        ComPtr<MockPrintControl> PrintControl;

        ComPtr<CanvasPrintEventArgs> Args;

        Fixture()
            : Device(Make<MockCanvasDevice>())
            , PrintTaskOptions(Make<MockPrintTaskOptions>())
            , AnyTarget(Make<MockPrintDocumentPackageTarget>())
            , PrintControl(Make<MockPrintControl>())
            , Args(Make<CanvasPrintEventArgs>(Device, AnyTarget, PrintTaskOptions, AnyDpi))
        {
            Device->CreatePrintControlMethod.AllowAnyCall(
                [=] (IPrintDocumentPackageTarget*, float)
                {
                    return PrintControl;
                });

            Device->CreateCommandListMethod.AllowAnyCall(
                [=]
                {
                    auto cl = Make<MockD2DCommandList>();
                    cl->CloseMethod.SetExpectedCalls(1);
                    return cl;
                });

            Device->CreateDeviceContextForDrawingSessionMethod.AllowAnyCall(
                [=]
                {
                    return Make<StubD2DDeviceContext>(nullptr);
                });

            PrintTaskOptions->GetPageDescriptionMethod.AllowAnyCall();
            PrintControl->AddPageMethod.AllowAnyCall();
        }

    };

    TEST_METHOD_EX(CanvasPrintEventArgs_Getters_FailWithBadParams)
    {
        Fixture f;

        Assert::AreEqual(E_INVALIDARG, f.Args->get_PrintTaskOptions(nullptr));
        Assert::AreEqual(E_INVALIDARG, f.Args->get_Dpi(nullptr));
        Assert::AreEqual(E_INVALIDARG, f.Args->GetDeferral(nullptr));
        Assert::AreEqual(E_INVALIDARG, f.Args->CreateDrawingSession(nullptr));
    }

    TEST_METHOD_EX(CanvasPrintEventArgs_DpiCanBeModified)
    {
        Fixture f;

        float expectedDpi = AnyDpi * 2;

        ThrowIfFailed(f.Args->put_Dpi(expectedDpi));

        float retrievedDpi;
        ThrowIfFailed(f.Args->get_Dpi(&retrievedDpi));

        Assert::AreEqual(expectedDpi, retrievedDpi);
    }

    TEST_METHOD_EX(CanvasPrintEventArgs_DpiMustBeGreaterThanZero)
    {
        Fixture f;

        Assert::AreEqual(E_INVALIDARG, f.Args->put_Dpi(0.0f));
        Assert::AreEqual(E_INVALIDARG, f.Args->put_Dpi(-FLT_EPSILON));
        Assert::AreEqual(E_INVALIDARG, f.Args->put_Dpi(-1000.0f));
        Assert::AreEqual(S_OK, f.Args->put_Dpi(FLT_EPSILON));
    }

    TEST_METHOD_EX(CanvasPrintEventArgs_CreateDrawingSession_CreatesPrintControlWithTheCorrectDpi)
    {
        Fixture f;

        float expectedDpi = AnyDpi * 2;
        ThrowIfFailed(f.Args->put_Dpi(expectedDpi));

        f.Device->CreatePrintControlMethod.SetExpectedCalls(1,
            [&] (IPrintDocumentPackageTarget*, float dpi)
            {
                Assert::AreEqual(expectedDpi, dpi);
                return f.PrintControl;
            });

        ComPtr<ICanvasDrawingSession> ds;
        ThrowIfFailed(f.Args->CreateDrawingSession(&ds));
    }

    TEST_METHOD_EX(CanvasPrintEventArgs_AfterFirstCreateDrawingSession_PutDpiFails)
    {
        Fixture f;

        ComPtr<ICanvasDrawingSession> ds;
        ThrowIfFailed(f.Args->CreateDrawingSession(&ds));

        Assert::AreEqual(E_FAIL, f.Args->put_Dpi(AnyDpi));
        ValidateStoredErrorState(E_FAIL, Strings::CanvasPrintEventArgsDpiCannotBeChangedAfterCreateDrawingSession);
    }

    TEST_METHOD_EX(CanvasPrintEventArgs_CreateDrawingSession_ReturnsDrawingSessionWithCorrectDpi)
    {
        Fixture f;

        ThrowIfFailed(f.Args->put_Dpi(AnyDpi));

        ComPtr<ICanvasDrawingSession> ds;
        ThrowIfFailed(f.Args->CreateDrawingSession(&ds));

        float dpi;
        ThrowIfFailed(As<ICanvasResourceCreatorWithDpi>(ds)->get_Dpi(&dpi));

        Assert::AreEqual(AnyDpi, dpi);
    }

    TEST_METHOD_EX(CanvasPrintEventArgs_WhenDrawingSessionIsClosed_CommandListPassedToPrintControl)
    {
        Fixture f;

        for (auto pageNumber = 1U; pageNumber < 10U; ++pageNumber)
        {
            float pageWidth = 100.0f * (float)pageNumber;
            float pageHeight = 200.0f * (float)pageNumber;
            
            auto printPageDescription = PrintPageDescription{
                Size{ pageWidth, pageHeight },
                Rect{ 0, 0, pageWidth, pageHeight }, // ImageableRect
                (uint32_t)AnyDpi, (uint32_t)AnyDpi   // DPI X, Y
            };

            f.PrintTaskOptions->GetPageDescriptionMethod.AllowAnyCall(
                [&] (uint32_t page, PrintPageDescription* outDesc)
                {
                    Assert::AreEqual(pageNumber, page);
                    *outDesc = printPageDescription;
                    return S_OK;
                });

            ComPtr<ICanvasDrawingSession> ds;
            ThrowIfFailed(f.Args->CreateDrawingSession(&ds));
            
            ComPtr<ID2D1Image> d2dTarget;
            GetWrappedResource<ID2D1DeviceContext>(ds)->GetTarget(&d2dTarget);
            
            f.PrintControl->AddPageMethod.SetExpectedCalls(1,
                [&] (ID2D1CommandList* commandList, D2D_SIZE_F pageSize, IStream* pagePrintTicketStream, D2D1_TAG* tag1, D2D1_TAG* tag2)
                {
                    Assert::IsTrue(IsSameInstance(d2dTarget.Get(), commandList));
                    Assert::AreEqual(D2D_SIZE_F{ pageWidth, pageHeight }, pageSize);
                    Assert::IsNull(pagePrintTicketStream);
                    Assert::IsNull(tag1);
                    Assert::IsNull(tag2);
                    return S_OK;
                });
            
            ThrowIfFailed(As<IClosable>(ds)->Close());
        }
    }

    TEST_METHOD_EX(CanvasPrintEventArgs_WhenCreateDrawingSession_IsCalledBeforeLastDrawingSessionClose_ItFails)
    {
        Fixture f;

        ComPtr<ICanvasDrawingSession> ds[2];
        ThrowIfFailed(f.Args->CreateDrawingSession(&ds[0]));

        Assert::AreEqual(E_FAIL, f.Args->CreateDrawingSession(&ds[1]));
        ValidateStoredErrorState(E_FAIL, Strings::CanvasPrintEventArgsCannotCreateDrawingSessionUntilPreviousOneClosed);
    }

    // TODO #5659: Verify failure behavior (including device lost, in paginate/makepage/makedocument)
};


