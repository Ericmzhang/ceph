import { TestBed } from '@angular/core/testing';

import { StorageInsightsService } from './storage-insights.service';
import { HttpClientTestingModule } from '@angular/common/http/testing';
import { RouterTestingModule } from '@angular/router/testing';
import { configureTestBed } from '~/testing/unit-test-helper';
import { SharedModule } from '~/app/shared/shared.module';

describe('StorageInsightsService', () => {
  let service: StorageInsightsService;

  configureTestBed({
    imports: [HttpClientTestingModule, RouterTestingModule, SharedModule],
    providers: [StorageInsightsService]
  });

  beforeEach(() => {
    TestBed.configureTestingModule({});
    service = TestBed.inject(StorageInsightsService);
  });

  it('should be created', () => {
    expect(service).toBeTruthy();
  });
});
